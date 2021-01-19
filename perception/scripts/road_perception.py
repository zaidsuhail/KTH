#!/usr/bin/env python

# Descrition: Publishes local path along with estimated friction along it

# subscribes:
# state from stateestimation node (topic /state)
# global path from track interface (topic /pathglobal)

# publishes: 
# local path (topic /pathlocal)
# visualization of local path (/pathlocal_vis)

import numpy as np
import rospy
from common.msg import State
from common.msg import Path

from util import angleToInterval
from util import angleToContinous
from coordinate_transforms import ptsFrenetToCartesian
from geometry_msgs.msg import PoseStamped
from nav_msgs.msg import Path as navPath
from geometry_msgs.msg import Point32
from geometry_msgs.msg import PolygonStamped
from jsk_recognition_msgs.msg import PolygonArray
import time

import GPy

class RoadPerception:
    # constructor
    def __init__(self):
        # init node subs pubs
        rospy.init_node('road_perception', anonymous=True)
        self.pathglobalsub = rospy.Subscriber("pathglobal", Path, self.pathglobal_callback)
        self.pathglobal = Path()
        self.received_pathglobal = False
        
        self.state_sub = rospy.Subscriber("state", State, self.state_callback)
        self.state = State()
        self.received_state = False
        
        self.pathlocalpub = rospy.Publisher('pathlocal', Path, queue_size=10)
        self.pathlocal = Path()
        
        self.pathlocalvispub = rospy.Publisher('pathlocal_vis', PolygonArray, queue_size=1)

        # node params
        self.dt = rospy.get_param('/dt_road_perception')
        self.rate = rospy.Rate(1/self.dt)
        
        # params of local path
        self.N = rospy.get_param('/N_pathlocal')
        self.ds = rospy.get_param('/ds_pathlocal')
        self.s_rel_start_pathlocal = rospy.get_param('/s_rel_start_pathlocal')
        
        # params of friction est emulation
        self.mu_est_mode = rospy.get_param('/mu_est_mode')
        self.cons_level = rospy.get_param('/conservativeness_level')
        self.set_mu_est_errors() 
        
        # set static vehicle params
        self.setRosParams()

        # local vars
        self.pathrolling = Path() # used to run several laps
        self.kern = GPy.kern.RBF(input_dim=1, variance=1., lengthscale=10.)

        # wait for messages before entering main loop
        while(not self.received_pathglobal):
            print "perception: waiting for pathglobal"
            self.rate.sleep()
        self.maxmu = np.max(self.pathglobal.mu)
        
        
        while(not self.received_state):
            print "perception: waiting for state"
            self.rate.sleep()

        print "road perception: running main with: "
        print "road perception: lap length: ", self.s_lap
        print "road perception: length of local path: ", self.N*self.ds
        
        # Main loop
        while not rospy.is_shutdown():
            
            # check timing wrt dt
            start = time.time()
 
            # update pathrolling to handle multiple laps
            if (self.state.s > self.pathrolling.s[0]+self.s_lap + 25): # if we're on the second lap of pathrolling
                self.pathrolling.s = self.pathrolling.s + self.s_lap
           
            # update local path 
            self.updateLocalPath()
            print self.pathlocal.s
            self.pathlocalpub.publish(self.pathlocal)
            
            # visualize local path in rviz
            pathlocal_vis = navPath()
            pathlocal_vis.header.stamp = rospy.Time.now()
            pathlocal_vis.header.frame_id = "map"
            for i in range(self.N):
                pose = PoseStamped()
                pose.header.stamp = rospy.Time.now()
                pose.header.frame_id = "map"
                pose.pose.position.x = self.pathlocal.X[i]
                pose.pose.position.y = self.pathlocal.Y[i]       
                pathlocal_vis.poses.append(pose)

            pa = self.pathLocalToPolArr()
            self.pathlocalvispub.publish(pa)

            end = time.time()
            comptime = end-start
            if (comptime > self.dt):
                rospy.logwarn("perception: compute time exceeding dt!")

            self.rate.sleep()   
            

    def updateLocalPath(self):
       
        stot_local = self.N*self.ds
        #self.smin_local = max(self.state.s-1, 0.0)
        self.smin_local = self.state.s-self.s_rel_start_pathlocal
        # at the start - make sure smin_local > 0
        if(self.smin_local < 0):
            self.smin_local = 0 

        smax_local = self.smin_local+stot_local
        s = np.linspace(self.smin_local,smax_local,self.N)
        
        if (s[0] < self.pathrolling.s[0]):
            rospy.logerr("perception: pathlocal.s not on pathrolling interval! (pathlocal is behind)")
            rospy.logerr("smin_pathlocal = " + str(s[0]) + ", pathrolling.s[0] = " + str(self.pathrolling.s[0]))
        if (s[-1] > self.pathrolling.s[-1]):
            rospy.logerr("perception: pathlocal.s not on pathrolling interval! (pathlocal is ahead)")
            rospy.logerr("smax_pathlocal = " + str(s[-1]) + ", pathrolling.s[-1] = " + str(self.pathrolling.s[-1]))
        
        # interpolate on global path            
        self.pathlocal.header.stamp = rospy.Time.now()
        self.pathlocal.header.frame_id = "map"
        self.pathlocal.X =              np.interp(s,self.pathrolling.s,self.pathrolling.X)
        self.pathlocal.Y =              np.interp(s,self.pathrolling.s,self.pathrolling.Y)
        self.pathlocal.s =              s
        self.pathlocal.psi_c =          np.interp(s,self.pathrolling.s,angleToContinous(self.pathrolling.psi_c)) # interpolating on continous psic
        self.pathlocal.psi_c =          angleToInterval(self.pathlocal.psi_c)
        self.pathlocal.theta_c =        np.interp(s,self.pathrolling.s,self.pathrolling.theta_c)
        self.pathlocal.kappa_c =        np.interp(s,self.pathrolling.s,self.pathrolling.kappa_c)
        self.pathlocal.kappaprime_c =   np.interp(s,self.pathrolling.s,self.pathrolling.kappaprime_c)
        
        self.pathlocal.dub =            np.interp(s,self.pathrolling.s,self.pathrolling.dub)
        self.pathlocal.dlb =            np.interp(s,self.pathrolling.s,self.pathrolling.dlb)

        # set mu        
        mu_gt = np.interp(s,self.pathrolling.s,self.pathrolling.mu)
        self.pathlocal.mu = self.emulateFrictionEst(self.pathlocal.s,
                                                    mu_gt,
                                                    self.mu_est_mode,
                                                    self.cons_level,
                                                    self.local_est_error,
                                                    self.predictive_est_error)   


    def emulateFrictionEst(self,s_pl,mu_gt,mu_est_mode,cons_level,local_est_error,predictive_est_error):
        
        # TODO conditions for available local est
        # TODO compute cam error from classes
        
        N_pl_50 = int(50.0/self.ds)
        s_pl_50 = s_pl[0:N_pl_50]
        mu_gt_50 = mu_gt[0:N_pl_50]
        
        if(mu_est_mode == 0): # GT  
            return mu_gt
        if(mu_est_mode == 1): # local only (GT)
            mu_est_at_ego = np.interp(self.state.s,s_pl,mu_gt)
            return mu_est_at_ego*np.ones_like(mu_gt)
        if(mu_est_mode == 2): # local with error
            mu_est_at_ego = np.interp(self.state.s,s_pl,mu_gt) + local_est_error
            return mu_est_at_ego*np.ones_like(mu_gt)
        if(mu_est_mode == 3): # predictive with error (camera)
            return mu_gt + predictive_est_error
        if(mu_est_mode == 4): # GP merge
            
            
            abs_errors_Y = 0.025*np.ones_like(s_pl_50)
            abs_errors_Y[0] = 0.0001
            abs_errors_Y[1] = 0.0001
            mean, std_dev = self.het_gp_regression(self.kern, s_pl_50, mu_gt_50, abs_errors_Y)
            
            mu_est = mu_gt
            mu_est[0:N_pl_50] = mean.reshape(1, -1)
            return mu_est 

    def set_mu_est_errors(self):
        # todo set randomly from some distribution
        self.local_est_error = -0.05
        self.predictive_est_error = -0.2 

    def het_gp_regression(self, kern, X_train, Y_train, abs_errors_Y):
        m = GPy.models.GPHeteroscedasticRegression(X_train[:,None],Y_train[:,None],kern)
        m['.*het_Gauss.variance'] = abs_errors_Y[:,None] #Set the noise parameters to the error in Y
        mu, var = m._raw_predict(m.X)
        
        return mu, np.sqrt(var)

    def pathLocalToPolArr(self):
        pa = PolygonArray()
        pa.header.stamp = rospy.Time.now()
        pa.header.frame_id = "map"
        for i in range(self.N-1):

            spoly = np.array([self.pathlocal.s[i], self.pathlocal.s[i+1],self.pathlocal.s[i+1],self.pathlocal.s[i]])
            dpoly = np.array([self.pathlocal.dub[i], self.pathlocal.dub[i+1],self.pathlocal.dlb[i+1],self.pathlocal.dlb[i]])
            Xpoly, Ypoly = ptsFrenetToCartesian(spoly,dpoly,self.pathlocal.X,self.pathlocal.Y,self.pathlocal.psi_c,self.pathlocal.s)

            p = PolygonStamped()
            p.header.stamp = rospy.Time.now()
            p.header.frame_id = "map"
            p.polygon.points = [Point32(x=Xpoly[0], y=Ypoly[0], z=0.0),
                                Point32(x=Xpoly[1], y=Ypoly[1], z=0.0),
                                Point32(x=Xpoly[2], y=Ypoly[2], z=0.0),
                                Point32(x=Xpoly[3], y=Ypoly[3], z=0.0)]


            pa.polygons.append(p)
            #pa.Color.r = 1.0
            #p.color.g = 0.0
            #p.color.b = 0.0        
        
        # color for mu
        pa.likelihood = self.pathlocal.mu*(0.2/self.maxmu) # discarding final value
               
        #pa.color.r = 1.0
        #pa.color.g = 0.0
        #pa.color.b = 0.0
        
        return pa
 
    def setRosParams(self):
        self.m = rospy.get_param('/car/inertia/m')
        self.g = rospy.get_param('/car/inertia/g')
        self.lf = rospy.get_param('/car/kinematics/b_F')
        self.lr = rospy.get_param('/car/kinematics/b_R')
              
    def pathglobal_callback(self, msg):
        self.pathglobal = msg
        
        # get s of one lap
        stot_global = self.pathglobal.s[-1]
        dist_sf = np.sqrt( (self.pathglobal.X[0]-self.pathglobal.X[-1])**2 + (self.pathglobal.Y[0]-self.pathglobal.Y[-1])**2)
        self.s_lap = stot_global + dist_sf  
        
        # start pathrolling as two first laps
        if(not self.received_pathglobal):
            
            self.pathrolling.X = np.concatenate((np.array(self.pathglobal.X),np.array(self.pathglobal.X)),axis=0)
            self.pathrolling.Y = np.concatenate((np.array(self.pathglobal.Y),np.array(self.pathglobal.Y)),axis=0)
            self.pathrolling.s = np.concatenate((np.array(self.pathglobal.s),np.array(self.pathglobal.s) + self.s_lap ),axis=0)
            self.pathrolling.psi_c = np.concatenate((np.array(self.pathglobal.psi_c),np.array(self.pathglobal.psi_c)),axis=0)
            self.pathrolling.theta_c = np.concatenate((np.array(self.pathglobal.theta_c),np.array(self.pathglobal.theta_c)),axis=0)
            self.pathrolling.kappa_c = np.concatenate((np.array(self.pathglobal.kappa_c),np.array(self.pathglobal.kappa_c)),axis=0)
            self.pathrolling.kappaprime_c = np.concatenate((np.array(self.pathglobal.kappaprime_c),np.array(self.pathglobal.kappaprime_c)),axis=0)
            self.pathrolling.dub = np.concatenate((np.array(self.pathglobal.dub),np.array(self.pathglobal.dub)),axis=0)
            self.pathrolling.dlb = np.concatenate((np.array(self.pathglobal.dlb),np.array(self.pathglobal.dlb)),axis=0)
        
        # mu can be updated later by exp manager
        self.pathrolling.mu = np.concatenate((np.array(self.pathglobal.mu),np.array(self.pathglobal.mu)),axis=0)
        
        self.received_pathglobal = True
    
    def state_callback(self, msg):
        self.state = msg
        self.received_state = True

if __name__ == '__main__':
    rp = RoadPerception()
    try:
        rospy.spin()
    except KeyboardInterrupt:
        print("Shutting down")   
    
    
