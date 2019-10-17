#include "saarti/saarti_node.h"

namespace saarti_node{

SAARTI::SAARTI(ros::NodeHandle nh){
    nh_ = nh;
    // node rate
    dt_ = nh_.param("/dt", 0.1);
    ros::Rate loop_rate(1/dt_);

    // load rosparams
    get_rosparams();

    // pubs & subs
    trajhat_pub_ = nh.advertise<common::Trajectory>("trajhat",1);
    trajstar_pub_ = nh.advertise<common::Trajectory>("trajstar",1);
    pathlocal_sub_ = nh.subscribe("pathlocal", 1, &SAARTI::pathlocal_callback,this);
    obstacles_sub_ = nh.subscribe("obstacles", 1, &SAARTI::obstacles_callback,this);
    state_sub_ = nh.subscribe("state", 1,  &SAARTI::state_callback,this);
    // visualization
    trajhat_vis_pub_ = nh.advertise<nav_msgs::Path>("trajhat_vis",1);
    trajstar_vis_pub_ = nh.advertise<nav_msgs::Path>("trajstar_vis",1);
    trajset_vis_pub_ = nh.advertise<visualization_msgs::Marker>("trajset_vis",1);
    posconstr_vis_pub_ = nh.advertise<jsk_recognition_msgs::PolygonArray>("posconstr_vis",1);
    vectordebug_pub_ = nh.advertise<jsk_recognition_msgs::PlotData>("saarti_plot_debug",1);

    // init wrapper for rtisqp solver
    rtisqp_wrapper_ = RtisqpWrapper();
    // set weights
    rtisqp_wrapper_.setWeights(Wx_,WNx_,Wu_,Wslack_);

    // wait until state and path_local is received
    while( (state_.s < 0) || pathlocal_.s.size() == 0){
        ROS_INFO_STREAM("waiting for state and/or path local");
        if(state_.s < 0){
            ROS_ERROR_STREAM("state.s is negative! s = " << state_.s);
        }
        ros::spinOnce();
        loop_rate.sleep();
    }

    // initialize trajhat last
    planning_util::trajstruct trajstar_last;

    // main loop
    while (ros::ok())
    {
        ROS_INFO_STREAM(" ");
        ROS_INFO_STREAM("main_ loop_");
        auto t1_loop = std::chrono::high_resolution_clock::now();

        // update estimate
//        for (uint k=0;k<N;k++) {
//            Ukt_.Fyf_lb.push_back(-500);
//            Ukt_.Fyf_ub.push_back(500);
//            Ukt_.Fx_lb.push_back(-1000);
//            Ukt_.Fx_ub.push_back(1000);
//        }

        /*
         * GENERATE FEASIBLE INITIAL GUESS
         */

        // set refs
        refs_ = setRefs(ref_mode_); // 0: min s, 1: max s,

        ROS_INFO_STREAM("selecting initial guess");
        trajset_.clear();
        planning_util::trajstruct trajhat;
        auto t1_rollout = std::chrono::high_resolution_clock::now();

        // regular RTI (initialize with single rollout)
        if (algo_setting_== 0) {
            planning_util::trajstruct trajprime;
            if (trajstar_last.s.size()==0){
                ROS_INFO_STREAM("generating init traj for RTISQP");
                for (uint i=0;i<N;i++) {
                    trajprime.Fyf.push_back(0);
                    trajprime.Fxf.push_back(300); // todo get from mu
                    trajprime.Fxr.push_back(300);
                }
                rtisqp_wrapper_.rolloutSingleTraj(trajprime,state_,pathlocal_,sp_);
                trajset_.push_back(trajprime);
            } else{ // set trajprime as initial guess
                //rtisqp_wrapper_.shiftTrajectoryFwdSimple(trajstar_last);
                trajprime = rtisqp_wrapper_.shiftTrajectoryByIntegration(trajstar_last,state_,pathlocal_,sp_);
                trajset_.push_back(trajprime);
            }
            trajhat = trajprime;
        }

        // SAARTI
        if(algo_setting_ == 1){
            ROS_INFO_STREAM("generating trajectory set");
            rtisqp_wrapper_.computeTrajset(trajset_,state_,pathlocal_,uint(Ntrajs_rollout_));

            // cost eval and select
            int trajhat_idx = trajset_eval_cost(); // error if negative
            if(trajhat_idx >= 0){
                trajhat = trajset_.at(uint(trajhat_idx));
            } else {
                ROS_ERROR_STREAM("saarti traj select; no traj selected, idx negative");
            }

        }

        // sanity check on trajhat
        bool hasnans = false;
        for(uint k=0; k<N;k++){
            if(std::isnan(trajhat.s.at(k))){
                hasnans = true;
            }
        }
        if(hasnans){
            ROS_ERROR("Initial guess selection failed, breaking loop: trajhat has nans");
            break;
        }

        // get cartesian coords
        traj2cart(trajhat);

        // only for visualization, comment out to save time
        trajset2cart();
        visualization_msgs::Marker trajset_cubelist = trajset2cubelist();

        auto t2_rollout = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> t_rollout = t2_rollout - t1_rollout;


        // prints on trajhat
        //ROS_INFO_STREAM("trajhat.cost = " << trajhat.cost);
        nav_msgs::Path p_trajhat = traj2navpath(trajhat);

        if(trajhat.s.back() > 0.95f*pathlocal_.s.back()){
            ROS_ERROR_STREAM("Running out of path!");
            ROS_ERROR_STREAM("trajhat.s.back() = " << trajhat.s.back());
            ROS_ERROR_STREAM("pathlocal_.s.back() = " << pathlocal_.s.back());
        }

        // debug pathlocal and trajhat
        jsk_recognition_msgs::PlotData pd;
//        pd.xs = trajhat.s;
//        pd.ys = trajhat.kappac;
//        pd.label = "trajhat.kappac";
        pd.xs = cpp_utils::linspace(0.0f,1.0f,N);
        pd.ys = trajhat.s;
        pd.label = "trajhat.s";
        pd.type = jsk_recognition_msgs::PlotData::SCATTER;
        vectordebug_pub_.publish(pd);

        /*
         * OPTIMIZATION
         */

        // update adaptive constraints for opt
        //rtisqp_wrapper_.setInputConstraints(0.3f,1000); // todo input static params etc

        // update state for opt
        ROS_INFO_STREAM("setting state..");
        // debug - assuming correct deltapsi
        //        planning_util::statestruct plannedstate;
        //        planning_util::state_at_idx_in_traj(trajhat,plannedstate,1);
        //        state_.deltapsi = plannedstate.deltapsi;
        rtisqp_wrapper_.setInitialState(state_);

        // check trajhat w.r.t zerodivision
        for (uint k=0; k<trajhat.s.size(); k++){
            if (std::abs(1.0f - trajhat.d.at(k)*trajhat.kappac.at(k)) < 0.1f){
                ROS_ERROR_STREAM("DIVISION BY ZERO IN TRAJHAT DYNAMICS: 1-d*kappac =" << 1.0f - trajhat.d.at(k)*trajhat.kappac.at(k) );
            }
        }

        // set initial guess
        ROS_INFO_STREAM("setting initial guess..");
        rtisqp_wrapper_.setInitialGuess(trajhat);

        // set refs in solver
        ROS_INFO_STREAM("setting reference..");
        rtisqp_wrapper_.setOptReference(trajhat,refs_);

        // set state constraint
        ROS_INFO_STREAM("setting state constraints..");
        vector<float> lld = cpp_utils::interp(trajhat.s,pathlocal_.s,pathlocal_.dub,false);
        vector<float> rld = cpp_utils::interp(trajhat.s,pathlocal_.s,pathlocal_.dlb,false);
        float w = 1.75; // TODO get from param
        planning_util::posconstrstruct posconstr = rtisqp_wrapper_.setStateConstraints(trajhat,obst_,lld,rld,w);
        // visualize state constraint
        jsk_recognition_msgs::PolygonArray polarr = stateconstr2polarr(posconstr);

        // run optimization in separate thread for timeout option
        auto t1_opt = std::chrono::high_resolution_clock::now();

        std::thread t (&SAARTI::run_optimization,this);
        t.join();

        // Terminate the thread.
        int dtms = int(dt_*1000);
        auto future = std::async(std::launch::async, &std::thread::join, &t);
        if (future.wait_for(std::chrono::milliseconds(dtms)) == std::future_status::timeout) {
            ROS_ERROR_STREAM("OPTIMIZATION TIMED OUT");
            break; // todo, reinitialize instead
        }

        auto t2_opt = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> t_opt = t2_opt - t1_opt;

        // extract trajstar from acado
        planning_util::trajstruct trajstar = rtisqp_wrapper_.getTrajectory();
        traj2cart(trajstar);
        nav_msgs::Path p_trajstar = traj2navpath(trajstar);

        // check trajstar w.r.t zerodivision
        bool publish_trajs = true;
        for (uint k=0; k<trajstar.s.size(); k++){
            if (std::abs(1.0f - trajstar.d.at(k)*trajstar.kappac.at(k)) < 0.1f){
                ROS_ERROR_STREAM("DIVISION BY ZERO IN TRAJSTAR DYNAMICS, STOP PUBLISHING: 1-d*kappac =" << 1.0f - trajstar.d.at(k)*trajstar.kappac.at(k) );
                publish_trajs = false;
                break;
            }
        }

        /*
         * PUBLISH
         */

        // publish trajhat
        common::Trajectory trajhat_msg = traj2msg(trajhat);
        trajhat_msg.slb = posconstr.slb;
        trajhat_msg.sub = posconstr.sub;
        trajhat_msg.dlb = posconstr.dlb;
        trajhat_msg.dub = posconstr.dub;
        trajhat_msg.header.stamp = ros::Time::now();
        if(publish_trajs){
            trajhat_pub_.publish(trajhat_msg);
        }

        // publish trajstar
        common::Trajectory trajstar_msg = traj2msg(trajstar);
        trajstar_msg.header.stamp = ros::Time::now();
        if(publish_trajs){
            trajstar_pub_.publish(trajstar_msg);
        }

        // publish visualization msgs
        if(publish_trajs){
            trajhat_vis_pub_.publish(p_trajhat);
            trajstar_vis_pub_.publish(p_trajstar);
            trajset_vis_pub_.publish(trajset_cubelist);
            posconstr_vis_pub_.publish(polarr);
        }

        // store trajstar for next iteration
        trajstar_last = trajstar;

        // print timings
        ROS_INFO_STREAM("planning iteration complete, Timings: ");
        auto t2_loop = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> t_loop = t2_loop - t1_loop;
        if(t_loop.count() > dt_*1000 ){
            ROS_WARN_STREAM("looptime exceeding dt! looptime is " << t_loop.count() << " ms ");
        } else{
            ROS_INFO_STREAM("looptime is " << t_loop.count() << " ms ");
        }
        ROS_INFO_STREAM("rollout took                 " << t_rollout.count() << " ms " << "(" << Ntrajs_rollout_ << "trajs)");
        ROS_INFO_STREAM("optimization took            " << t_opt.count() << " ms ");

        ros::spinOnce();
        loop_rate.sleep();
    }
}


/*
 * FUNCTIONS todo put some in util
 */

// sets refs to be used in rollout and optimization
planning_util::refstruct SAARTI::setRefs(int ctrlmode){
    planning_util::refstruct refs;
    switch (ctrlmode) {
    case 0:  // minimize vx (emg brake)
        refs.sref.assign(N+1,state_.s);
        refs.vxref.assign(N+1,0.0);
        break;
    case 1: // maximize s (racing)
        refs.sref.assign(N+1, state_.s + 300);
        refs.vxref.assign(N+1, state_.vx + 25);
        break;
    }
    return refs;
}

// computes cartesian coordinates of a trajectory
void SAARTI::traj2cart(planning_util::trajstruct &traj){
    if(!traj.s.size()){
        ROS_ERROR("traj2cart on traj of 0 length");
    }
    else {
        // clear previous cartesian if exists
        if (traj.X.size() != 0){
            ROS_WARN_STREAM("traj already has cartesian, clearing X, Y, psi, kappac");
            traj.X.clear();
            traj.Y.clear();
            traj.psi.clear();
            traj.kappac.clear();
        }

        // check that s is monotonically increasing and has reasonable steps
        float vmax = 30; // m/s todo get from param
        for (uint k=0; k<traj.s.size()-1;k++) {
            if (traj.s.at(k+1)-traj.s.at(k) <= 0 ) {
                ROS_ERROR_STREAM("trajhat.s is not monotonically increasing");
                // todo dump pathlocal here to see if it has errors
                // todo 2 debug integrator

            }
            if (traj.s.at(k+1)-traj.s.at(k) >= float(dt_)*vmax ) {
                ROS_ERROR_STREAM("trajhat.s increases by more than vmax*dt, at index: " << k << ", value: " << traj.s.at(k+1)-traj.s.at(k));
            }
        }
        vector<float> Xc = cpp_utils::interp(traj.s,pathlocal_.s,pathlocal_.X,false);
        vector<float> Yc = cpp_utils::interp(traj.s,pathlocal_.s,pathlocal_.Y,false);
        // handle discontinuities in psic
        vector<float> pathlocal_psic_cont = angle_to_continous(pathlocal_.psi_c);
        vector<float> psic = cpp_utils::interp(traj.s,pathlocal_.s,pathlocal_psic_cont,false);
        angle_to_interval(psic); // bring traj.psic back to [-pi pi]

        for (uint j=0; j<traj.s.size();j++) {
            // X = Xc - d*sin(psic);
            // Y = Yc + d*cos(psic);
            // psi = deltapsi + psic;
            float X = Xc.at(j) - traj.d.at(j)*std::sin(psic.at(j));
            float Y = Yc.at(j) + traj.d.at(j)*std::cos(psic.at(j));
            float psi = traj.deltapsi.at(j) + psic.at(j);

            // build vectors
            traj.X.push_back(X);
            traj.Y.push_back(Y);
            traj.psi.push_back(psi);
        }
        traj.kappac = cpp_utils::interp(traj.s,pathlocal_.s,pathlocal_.kappa_c,false);
        // ensure psi is in [-pi pi]
        angle_to_interval(traj.psi);
    }
}

// computes cartesian coordinates of a trajectory set
void SAARTI::trajset2cart(){
    for (uint i=0;i<trajset_.size();i++) {
        traj2cart(trajset_.at(i));
    }
}

// computes cartesian coordinates of a set of s,d pts
void SAARTI::sd_pts2cart(vector<float> &s, vector<float> &d, vector<float> &Xout, vector<float> &Yout){
    vector<float> Xc = cpp_utils::interp(s,pathlocal_.s,pathlocal_.X,false);
    vector<float> Yc = cpp_utils::interp(s,pathlocal_.s,pathlocal_.Y,false);
    vector<float> pathlocal_psic_cont = angle_to_continous(pathlocal_.psi_c);
    vector<float> psic = cpp_utils::interp(s,pathlocal_.s,pathlocal_psic_cont,false);
    angle_to_interval(psic);

    for (uint j=0; j<s.size();j++) {
        // X = Xc - d*sin(psic);
        // Y = Yc + d*cos(psic);
        // psi = deltapsi + psic;
        float X = Xc.at(j) - d.at(j)*std::sin(psic.at(j));
        float Y = Yc.at(j) + d.at(j)*std::cos(psic.at(j));
        Xout.push_back(X);
        Yout.push_back(Y);
    }
}

// wraps an angle variable on the interval [-pi pi]
void SAARTI::angle_to_interval(vector<float> &psi){
    // default interval: [-pi pi]
    float pi = float(M_PI);
    for (uint i=0; i<psi.size(); i++){
        while(psi.at(i) > pi){
            psi.at(i) = psi.at(i) - 2*pi;
        }
        while(psi.at(i) <= -pi){
            psi.at(i) = psi.at(i) + 2*pi;
        }
    }
}

// unwraps an angle variable on the interval [-pi pi] to continous
vector<float> SAARTI::angle_to_continous(vector<float> &psi){
    float pi = float(M_PI);
    float offset = 0;
    vector<float> psi_cont;
    for (uint i=0;i<psi.size()-1;i++) {
        psi_cont.push_back(psi.at(i) + offset);
        if(psi.at(i+1) - psi.at(i) > pi){ // detecting up-flip
            offset = offset - 2*pi;
        }
        if(psi.at(i+1) - psi.at(i) < -pi){ // detecting down-flip
            offset = offset + 2*pi;
        }
    }
    psi_cont.push_back(psi.back() + offset); // final value
    if (psi_cont.size() != psi.size()){
        ROS_ERROR_STREAM("fault in angle_to_continous");
    }
    return psi_cont;
}

// cost evaluation and collision checking of trajset
int SAARTI::trajset_eval_cost(){
    float mincost = float(Wslack_)*10;
    int trajhat_idx = -1;
    for (uint i=0;i<trajset_.size();i++) {
        planning_util::trajstruct traj = trajset_.at(i);
        bool colliding = false;
        bool exitroad = false;
        float cost = 0;
        vector<float> dub = cpp_utils::interp(traj.s,pathlocal_.s,pathlocal_.dub,false);
        vector<float> dlb = cpp_utils::interp(traj.s,pathlocal_.s,pathlocal_.dlb,false);
        for (uint j=0; j<traj.s.size();j++){
            float s = traj.s.at(j);
            float d = traj.d.at(j);
            float vx = traj.vx.at(j);
            // check obstacle (in frenet)
            float dist;
            for (uint k=0; k<obst_.s.size();k++){
                dist = std::sqrt( (s-obst_.s.at(k))*(s-obst_.s.at(k)) + (d-obst_.d.at(k))*(d-obst_.d.at(k)) );
                if(dist < obst_.Rmgn.at(k)){
                    colliding = true;
                }
            }
            // check outside road (in frenet)
            if((d > dub.at(j)) || d < dlb.at(j) ){
                exitroad = true;
            }
            // running cost
            float sref = float(refs_.sref.at(j));
            float vxref = float(refs_.vxref.at(j));
            //cout << "sref before rc add = " << sref << endl;
            //cout << "vxref before rc add = " << vxref << endl;
            //cout << "s before rc add = " << s << endl;
            //cout << "vx before rc add = " << vx << endl;
            //cout << "cost before rc add = " << cost << endl;
            cost += (sref-s)*float(Wx_.at(0))*(sref-s) + (vxref-vx)*float(Wx_.at(4))*(vxref-vx);
            //cout << "cost after rc add = " << cost << endl;
        }
        if(colliding){
            cost += float(Wslack_);
            //cost = float(Wslack);
        }
        if(exitroad){
            cost += float(Wslack_);
            //cost = float(Wslack);
        }
        traj.cost = cost;
        //cout << "cost of traj nr " << i << ": " << cost << endl;
        traj.colliding = colliding;
        traj.exitroad = exitroad;

        // keep track of minimum cost traj
        if(cost < mincost){
            mincost = cost;
            trajhat_idx = int(i);
        }
    }
    return trajhat_idx;
}

// builds trajectry message from traj struct
common::Trajectory SAARTI::traj2msg(planning_util::trajstruct traj){
    common::Trajectory trajmsg;
    // state
    trajmsg.s = traj.s;
    trajmsg.d = traj.d;
    trajmsg.deltapsi = traj.deltapsi;
    trajmsg.psidot = traj.psidot;
    trajmsg.vx = traj.vx;
    trajmsg.vy = traj.vy;
    // ctrl
    trajmsg.Fyf = traj.Fyf;
    trajmsg.Fxf = traj.Fxf;
    trajmsg.Fxr = traj.Fxr;
    // cart pose
    trajmsg.X = traj.X;
    trajmsg.Y = traj.Y;
    trajmsg.psi = traj.psi;

    return trajmsg;
}

// represent traj as navmsgs path for visualization
nav_msgs::Path SAARTI::traj2navpath(planning_util::trajstruct traj){
    if (traj.X.size() == 0){
        ROS_ERROR_STREAM("No cartesan coordinates for traj" );
    }

    nav_msgs::Path p;
    p.header.stamp = ros::Time::now();
    p.header.frame_id = "map";
    for (uint i=0;i<traj.s.size();i++){
        geometry_msgs::PoseStamped pose;
        pose.header.stamp = ros::Time::now();
        pose.header.frame_id = "map";
        pose.pose.position.x = double(traj.X.at(i));
        pose.pose.position.y = double(traj.Y.at(i));
        tf2::Quaternion q;
        q.setRPY(0,0,double(traj.psi.at(i)));
        pose.pose.orientation.w = q.w();
        pose.pose.orientation.x = q.x();
        pose.pose.orientation.y = q.y();
        pose.pose.orientation.z = q.z();
        p.poses.push_back(pose);
    }
    return p;
}

// represent trajset as cubelist for fast rendering visualization
visualization_msgs::Marker SAARTI::trajset2cubelist(){
    visualization_msgs::Marker m;
    m.type = visualization_msgs::Marker::CUBE_LIST;
    m.scale.x = 0.1;
    m.scale.y = 0.1;
    m.scale.z = 0.05;
    m.color.a = 1.0;
    m.color.r = 0.0;
    m.color.g = 0.0;
    m.color.b = 1.0;
    m.header.stamp = ros::Time::now();
    m.header.frame_id = "map";
    m.pose.orientation.w = 1.0;
    for (uint i=0; i<trajset_.size(); i++) {
        planning_util::trajstruct traj = trajset_.at(i);
        for (uint j=0; j<traj.s.size();j++) {
            geometry_msgs::Point pt;
            pt.x = double(traj.X.at(j));
            pt.y = double(traj.Y.at(j));
            m.points.push_back(pt);
        }
    }
    return m;
}

// create visualization obj for state constraints
jsk_recognition_msgs::PolygonArray SAARTI::stateconstr2polarr(planning_util::posconstrstruct pc){
    jsk_recognition_msgs::PolygonArray polarr;
    polarr.header.stamp = ros::Time::now();
    polarr.header.frame_id = "map";
    for (uint i=0;i<pc.dlb.size();i+=5){
        geometry_msgs::PolygonStamped poly;
        poly.header.stamp = ros::Time::now();
        poly.header.frame_id = "map";
        vector<float> s{pc.slb.at(i),pc.sub.at(i),pc.sub.at(i),pc.slb.at(i)};
        vector<float> d{pc.dub.at(i),pc.dub.at(i),pc.dlb.at(i),pc.dlb.at(i)};
        vector<float> X;
        vector<float> Y;
        sd_pts2cart(s, d, X, Y);
        for (uint j=0;j<4;j++){
            geometry_msgs::Point32 pt;
            pt.x = X.at(j);
            pt.y = Y.at(j);
            poly.polygon.points.push_back(pt);
        }
        polarr.polygons.push_back(poly);
    }
    return polarr;
}

// state callback
void SAARTI::state_callback(const common::State::ConstPtr& msg){
    state_.s = msg->s;
    state_.d = msg->d;
    state_.deltapsi = msg->deltapsi;
    state_.psidot = msg->psidot;
    state_.vx = msg->vx;
    state_.vy = msg->vy;

    // curvilinear dynamics breaks when vx == 0
    float v_th = 1.0;
    if (state_.vx <= v_th){
        state_.vx = v_th;
    }

    // input checks on state
    //    if (std::abs(state_.deltapsi) > 0.2f){
    //        ROS_ERROR_STREAM("deltapsi large! deltapsi = " << state_.deltapsi );
    //    }
}

// pathlocal callback
void SAARTI::pathlocal_callback(const common::Path::ConstPtr& msg){
    pathlocal_.X = msg->X;
    pathlocal_.Y = msg->Y;
    pathlocal_.s = msg->s;
    pathlocal_.psi_c = msg->psi_c;
    pathlocal_.kappa_c = msg->kappa_c;
    pathlocal_.theta_c = msg->theta_c;
    pathlocal_.mu = msg->mu;
    pathlocal_.dub = msg->dub;
    pathlocal_.dlb = msg->dlb;
}

// obstacles callback
void SAARTI::obstacles_callback(const common::Obstacles::ConstPtr& msg){
    obst_.s = msg->s;
    obst_.d = msg->d;
    obst_.R = msg->R;
    obst_.Rmgn = msg->Rmgn;
}

// get static params from rosparam
void SAARTI::get_rosparams(){

    // opt config
    if(!nh_.getParam("/Wx", Wx_)){
        ROS_ERROR_STREAM("failed to load param Wx");
    }
    if(!nh_.getParam("/WNx", WNx_)){
        ROS_ERROR_STREAM("failed to load param WNx");
    }
    if(!nh_.getParam("/Wu", Wu_)){
        ROS_ERROR_STREAM("failed to load param Wu");
    }
    if(!nh_.getParam("/Wslack", Wslack_)){
        ROS_ERROR_STREAM("failed to load param Wslack");
    }

    // modes
    if(!nh_.getParam("/ref_mode", ref_mode_)){
        ROS_ERROR_STREAM("failed to load param /refmode");
    }
    if(!nh_.getParam("/algo_setting", algo_setting_)){
        ROS_ERROR_STREAM("failed to load param /algo_setting");
    }

    //nh_.getParam("/adaptive", adaptive_);

    // rollout config
    nh_.getParam("/Ntrajs_rollout", Ntrajs_rollout_);

    // static vehicle model params
    sp_.m =  float(nh_.param("/car/inertia/m",1000.0));
    sp_.g =  float(nh_.param("/car/inertia/g",9.81));
    sp_.lf = float(nh_.param("/car/kinematics/b_F",2.0));
    sp_.lr = float(nh_.param("/car/kinematics/b_R",2.0));
}

// run opt
void SAARTI::run_optimization(){
    // do preparation step
    ROS_INFO_STREAM("calling acado prep step..");
    rtisqp_wrapper_.doPreparationStep();

    // do feedback step
    ROS_INFO_STREAM("calling acado feedback step..");
    int status = rtisqp_wrapper_.doFeedbackStep();
    if (status){
        cout << "QP problem! QP status: " << status << endl;
    }
}

} // end namespace

int main(int argc, char **argv)
{
    ros::init(argc, argv, "saarti_node");
    ros::NodeHandle nh;
    saarti_node::SAARTI saarti(nh);
    return 0;
}
