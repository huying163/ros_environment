#!/usr/bin/env python
import rospy
import random
from geometry_msgs.msg import Point

def armor():
	pub = rospy.Publisher("cv_result",Point,queue_size=10)
	rospy.init_node('armor',anonymous=True)
	rate = rospy.Rate(2)
	while not rospy.is_shutdown():
			x = random.randrange(-5000,5000)
			y = random.randrange(-5000,5000)
			z = random.randrange(-5000,5000)
			st = "Publishing armor {:.3f}, {:.3f}, {:.3f}".format(x,y,z)
			rospy.loginfo(st)
			p = Point(x,y,z)
			pub.publish(p)
			rate.sleep()

if __name__ == '__main__':
	try:
		armor()
	except rospy.ROSInterruptException:
		pass
