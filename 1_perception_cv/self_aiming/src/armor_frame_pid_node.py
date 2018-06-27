#!/usr/bin/python

'''
This ros package uses the world coordinate result by "solvePnP".
'''
import roslib
import sys
import rospy
from geometry_msgs.msg import Twist
from rm_cv.msg import ArmorRecord
from can_receive_msg.msg import imu_16470
import numpy as np
import quaternion
import math
from collections import deque


def rotation_matrix(direction, angle):
    sina = math.sin(angle)
    cosa = math.cos(angle)
    # rotation matrix around unit vector
    R = np.diag([cosa, cosa, cosa])
    R += np.outer(direction, direction) * (1.0 - cosa)
    direction *= sina
    R += np.array([[0.0, -direction[2], direction[1]],
                   [direction[2], 0.0, -direction[0]],
                   [-direction[1], direction[0], 0.0]])
    M = np.identity(4)
    M[:3, :3] = R
    return M


class armor_frame_pid:
    def __init__(self):
        self.armor_subscriber = rospy.Subscriber(
            "/detected_armor", ArmorRecord, self.cv_callback, queue_size=1)
        self.imu_16470_subscriber = rospy.Subscriber(
            "/can_receive_1/imu_16470", imu_16470, self.imu_callback, queue_size=1)
        self.cmd_pub = rospy.Publisher('/cmd_vel', Twist, queue_size=1)

        self.y_err = 0
        self.z_err = 0
        self.prev_y_err = 0
        self.prev_z_err = 0

        self.time_queue = deque([rospy.get_rostime()])
        self.imu_queue = deque([np.quaternion(1, 0, 0, 0)])

    def imu_callback(self, subImu_16470):
        current_quaternion = np.quaternion(
            subImu_16470.quaternion[0], subImu_16470.quaternion[1], subImu_16470.quaternion[2], subImu_16470.quaternion[3])
        current_time = subImu_16470.header3.stamp
        self.imu_queue.append(current_quaternion)
        self.time_queue.append(current_time)
        rospy.loginfo("imu callback -- queue length: %d", len(self.imu_queue))

    def cv_callback(self, subArmorRecord):
        vel_msg = Twist()
        if abs(subArmorRecord.armorPose.linear.x) < sys.float_info.epsilon:
            vel_msg.angular.y = 0.0
            vel_msg.angular.z = 0.0
        else:
            image_time = subArmorRecord.header.stamp
            now_time = rospy.get_rostime()

            start_index = 1
            for i in range(len(self.time_queue)):
                if self.time_queue[i] < image_time:
                    continue
                else:
                    start_index = i
                    break

            for _ in range(start_index - 1):
                self.imu_queue.popleft()
                self.time_queue.popleft()

            final_quaternion = np.quaternion(1, 0, 0, 0)
            array = quaternion.as_float_array(final_quaternion)
            for i in range(len(self.time_queue) - 1):
                temp_quaternion = self.imu_queue[i + 1] / self.imu_queue[i]
                final_quaternion = final_quaternion * temp_quaternion

            final_rotation_matrix = quaternion.as_rotation_matrix(
                final_quaternion)
            rospy.loginfo("cv callback -- queue length: %d",
                          len(self.imu_queue))
            rospy.loginfo("cv callback -- quaternion %f, %f, %f, %f",
                          array[0], array[1], array[2], array[3])
            y_kp = 0.0
            y_kd = 0.0
            z_kp = 0.0
            z_kd = 0.0
            image_center_y = 300
            image_center_x = 400
            if rospy.has_param('/server_node/y_kp'):
                y_kp = rospy.get_param('/server_node/y_kp')
                y_kd = rospy.get_param('/server_node/y_kd')
                z_kp = rospy.get_param('/server_node/z_kp')
                z_kd = rospy.get_param('/server_node/z_kd')
                image_center_x = rospy.get_param('/server_node/center_x')
                image_center_y = rospy.get_param('/server_node/center_y')

            shield_T_camera = np.array(
                [subArmorRecord.armorPose.linear.x, subArmorRecord.armorPose.linear.y, subArmorRecord.armorPose.linear.z])
            opencv_rotation = np.array([[0, 0, 1],
                                        [-1, 0, 0],
                                        [0, 1, 0]])

            shield_T_camera_rot = opencv_rotation.dot(shield_T_camera)
            # for soldier 2
            # camera_T_gimbal = np.array([135, 0, 0])
            # for soldier 1
            # camera_T_gimbal = np.array([185, 0, 0])

            camera_T_gimbal = np.array([142, -45, 0])
            T = shield_T_camera_rot + camera_T_gimbal
            T = final_rotation_matrix.dot(T)

            normalized_T = T / np.linalg.norm(T)
            x_axis = np.array([1, 0, 0])
            axis = np.cross(normalized_T, x_axis)
            normalized_axis = axis / np.linalg.norm(axis)
            angle = np.arccos(x_axis.dot(normalized_T))

            R = rotation_matrix(normalized_axis, angle)
            R = np.transpose(R)
            T_euler0 = np.arctan2(R[1, 0], R[0, 0])
            # T_euler1 = np.arccos(R[1,0] / math.sin(T_euler0))
            T_euler1 = np.arcsin(-R[2, 0])
            T_euler2 = np.arctan2(R[2, 1], R[2, 2])

            rospy.loginfo(
                "armor center in gimbal rotation center: %f, %f, %f", T[0], T[1], T[2])
            rospy.loginfo("armor center euler angle zyx: %f, %f, %f",
                          T_euler0, T_euler1, T_euler2)

            self.y_err = T_euler1 - image_center_y
            self.z_err = T_euler0 - image_center_x
            vy = y_kp * self.y_err + y_kd * (self.y_err - self.prev_y_err)
            vz = z_kp * self.z_err + z_kd * (self.z_err - self.prev_z_err)
            self.prev_y_err = self.y_err
            self.prev_z_err = self.z_err

            vel_msg.angular.y = vy
            vel_msg.angular.z = vz
        self.cmd_pub.publish(vel_msg)


if __name__ == "__main__":
    rospy.init_node('armor_frame_pid_node')
    pid = armor_frame_pid()
    rospy.spin()
