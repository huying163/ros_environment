#include <opencv2/opencv.hpp>
#include "linux/videodev2.h"
#include "CamBase.hpp"
#include "V4LCamDriver.hpp"
#include "helpers.hpp"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string>
#include <math.h>
#include <time.h>
#include "ros/ros.h"
using namespace std;
using namespace cv;

V4LCamDriver::V4LCamDriver(const string &config_path)
	: CamBase(config_path){};

V4LCamDriver::~V4LCamDriver()
{
	close(fd);
	delete[] mb;
};

bool V4LCamDriver::loadDriverParameters(const FileStorage &fs)
{
	const FileNode &node = fs["V4LCamDriver"];
	fsHelper::readOrDefault(node["video_path"], video_path, std::string("/dev/video0"));
	fsHelper::readOrDefault(node["buffer_size"], buffer_size, 3);
	if (buffer_size < 3)
		buffer_size = 3;
	fsHelper::readOrDefault(node["capture_width"], capture_width, 0);
	fsHelper::readOrDefault(node["capture_height"], capture_height, 0);
	fsHelper::readOrDefault(node["format"], format, 0);
	fsHelper::readOrDefault(node["auto_exp"], auto_exp, false);
	fsHelper::readOrDefault(node["exposureTime"], exposureTime, 30);
	return !node.empty();
};

bool V4LCamDriver::storeDriverParameters(FileStorage &fs)
{
	fs << "V4LCamDriver"
	   << "{"
	   << "video_path" << video_path
	   << "buffer_size" << buffer_size
	   << "capture_width" << capture_width
	   << "capture_height" << capture_height
	   << "format" << format
	   << "auto_exp" << auto_exp
	   << "exposureTime" << exposureTime
	   << "}";
}

//TODO:
bool V4LCamDriver::setCamConfig()
{
	setExposureTime(this->auto_exp, this->exposureTime);
	setFormat(this->capture_width, this->capture_height, true);
	return true;
};
bool V4LCamDriver::getCamConfig()
{
	this->refreshVideoFormat();
	return true;
};

bool V4LCamDriver::initialize()
{
	if (fileExist(video_path))
	{
		fd = open(video_path.c_str(), O_RDWR);
		mb = new MapBuffer[buffer_size];
		setExposureTime(auto_exp, exposureTime);
		setFormat(capture_width, capture_height, 1);
		buffr_idx = 0;
		return true;
	}
	else
	{
		ROS_WARN("%s not found, no such camera exists", video_path.c_str());
		return false;
	}
};

bool V4LCamDriver::initMMap()
{
	struct v4l2_requestbuffers bufrequest = {0};
	bufrequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	bufrequest.memory = V4L2_MEMORY_MMAP;
	bufrequest.count = buffer_size;

	if (ioctl(fd, VIDIOC_REQBUFS, &bufrequest) < 0)
	{
		perror("VIDIOC_REQBUFS");
		return false;
	}

	for (int i = 0; i < buffer_size; ++i)
	{
		struct v4l2_buffer bufferinfo = {0};
		bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		bufferinfo.memory = V4L2_MEMORY_MMAP;
		bufferinfo.index = i; /* Queueing buffer index 0. */

		// Put the buffer in the incoming queue.
		if (ioctl(fd, VIDIOC_QUERYBUF, &bufferinfo) < 0)
		{
			perror("VIDIOC_QUERYBUF");
			return false;
		}

		mb[i].ptr = mmap(
			NULL,
			bufferinfo.length,
			PROT_READ | PROT_WRITE,
			MAP_SHARED,
			fd,
			bufferinfo.m.offset);
		mb[i].size = bufferinfo.length;

		if (mb[i].ptr == MAP_FAILED)
		{
			perror("MAP_FAILED");
			return false;
		}
		memset(mb[i].ptr, 0, bufferinfo.length);

		// Put the buffer in the incoming queue.
		memset(&bufferinfo, 0, sizeof(bufferinfo));
		bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		bufferinfo.memory = V4L2_MEMORY_MMAP;
		bufferinfo.index = i;
		if (ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0)
		{
			perror("VIDIOC_QBUF");
			return false;
		}
	}
	return true;
};

bool V4LCamDriver::startStream()
{
	//rectify time to ros standard (wall time)
	struct timespec uptime;
	clock_gettime(CLOCK_MONOTONIC, &uptime);
	ros::Time sysTime;
	sysTime.sec = uptime.tv_sec;
	sysTime.nsec = uptime.tv_nsec;
	toEpochOffset = ros::Time::now() - sysTime;

	cur_frame = 0;

	refreshVideoFormat();
	if (initMMap() == false)
		return false;

	__u32 type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
	{
		perror("VIDIOC_STREAMON");
		return false;
	}
	return true;
};

bool V4LCamDriver::closeStream()
{
	cur_frame = 0;
	buffr_idx = 0;
	__u32 type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_STREAMOFF, &type) < 0)
	{
		perror("VIDIOC_STREAMOFF");
		return false;
	}
	for (int i = 0; i < buffer_size; ++i)
	{
		munmap(mb[i].ptr, mb[i].size);
	}
	return true;
};

bool V4LCamDriver::setExposureTime(bool auto_exp, int t)
{
	if (auto_exp)
	{
		struct v4l2_control control_s;
		control_s.id = V4L2_CID_EXPOSURE_AUTO;
		control_s.value = V4L2_EXPOSURE_AUTO;
		if (xioctl(fd, VIDIOC_S_CTRL, &control_s) < 0)
		{
			ROS_INFO("V4LCamDriver: Set Auto Exposure error");
			return false;
		}
	}
	else
	{
		struct v4l2_control control_s;
		control_s.id = V4L2_CID_EXPOSURE_AUTO;
		control_s.value = V4L2_EXPOSURE_MANUAL;
		if (xioctl(fd, VIDIOC_S_CTRL, &control_s) < 0)
		{
			ROS_INFO("V4LCamDriver: Set Manual Exposure error");
			return false;
		}

		control_s.id = V4L2_CID_EXPOSURE_ABSOLUTE;
		control_s.value = t;
		if (xioctl(fd, VIDIOC_S_CTRL, &control_s) < 0)
		{
			ROS_INFO("V4LCamDriver: Set Exposure Time error");
			return false;
		}
	}
	ROS_INFO("V4LCamDriver: set exposure time to %d success", t);
	return true;
};

bool V4LCamDriver::setFormat(int width, int height, bool mjpg)
{
	ROS_INFO("Setting video format to %dx%d", width, height);
	cur_frame = 0;
	struct v4l2_format fmt = {0};
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	if (mjpg == true)
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	else
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;

	if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
	{
		printf("Setting Pixel Format\n");
		return false;
	}
	return true;
};

bool V4LCamDriver::refreshVideoFormat()
{
	struct v4l2_format fmt = {0};
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
	{
		perror("Querying Pixel Format\n");
		return false;
	}
	capture_width = fmt.fmt.pix.width;
	capture_height = fmt.fmt.pix.height;
	format = fmt.fmt.pix.pixelformat;
	return true;
}

bool V4LCamDriver::getVideoSize(int &width, int &height)
{
	if (capture_width == 0 || capture_height == 0)
	{
		if (refreshVideoFormat() == false)
			return false;
	}
	width = capture_width;
	height = capture_height;
	return true;
}

bool V4LCamDriver::setVideoFPS(int fps)
{
	struct v4l2_streamparm stream_param = {0};
	stream_param.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	stream_param.parm.capture.timeperframe.denominator = fps;
	stream_param.parm.capture.timeperframe.numerator = 1;

	if (-1 == xioctl(fd, VIDIOC_S_PARM, &stream_param))
	{
		printf("Setting Frame Rate\n");
		return false;
	}
	return true;
}

bool V4LCamDriver::setBufferSize(int bsize)
{
	if (buffer_size != bsize)
	{
		buffer_size = bsize;
		delete[] mb;
		mb = new MapBuffer[buffer_size];
	}
};
void V4LCamDriver::restartCapture()
{
	close(fd);
	fd = open(video_path.c_str(), O_RDWR);
	buffr_idx = 0;
	cur_frame = 0;
};

void V4LCamDriver::cvtRaw2Mat(const void *data, cv::Mat &image)
{
	if (format == V4L2_PIX_FMT_MJPEG)
	{
		cv::Mat src(capture_height, capture_width, CV_8UC3, (void *)data);
		image = cv::imdecode(src, 1);
	}
	else if (format == V4L2_PIX_FMT_YUYV)
	{
		cv::Mat yuyv(capture_height, capture_width, CV_8UC2, (void *)data);
		cv::cvtColor(yuyv, image, CV_YUV2BGR_YUYV);
	}
};

int V4LCamDriver::xioctl(int fd, int request, void *arg)
{
	int r;
	do
		r = ioctl(fd, request, arg);
	while (-1 == r && EINTR == errno);
	return r;
};

void V4LCamDriver::info()
{
	struct v4l2_capability caps = {};
	if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &caps))
	{
		perror("Querying Capabilities\n");
		return;
	}

	printf("Driver Caps:\n"
		   "  Driver: \"%s\"\n"
		   "  Card: \"%s\"\n"
		   "  Bus: \"%s\"\n"
		   "  Version: %d.%d\n"
		   "  Capabilities: %08x\n",
		   caps.driver,
		   caps.card,
		   caps.bus_info,
		   (caps.version >> 16) && 0xff,
		   (caps.version >> 24) && 0xff,
		   caps.capabilities);
	//get crop info
	struct v4l2_cropcap cropcap = {0};
	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl(fd, VIDIOC_CROPCAP, &cropcap))
	{
		perror("Querying Cropping Capabilities\n");
		return;
	}

	printf("Camera Cropping:\n"
		   "  Bounds: %dx%d+%d+%d\n"
		   "  Default: %dx%d+%d+%d\n"
		   "  Aspect: %d/%d\n",
		   cropcap.bounds.width, cropcap.bounds.height, cropcap.bounds.left, cropcap.bounds.top,
		   cropcap.defrect.width, cropcap.defrect.height, cropcap.defrect.left, cropcap.defrect.top,
		   cropcap.pixelaspect.numerator, cropcap.pixelaspect.denominator);

	//get video format info
	struct v4l2_fmtdesc fmtdesc = {0};
	fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	char fourcc[5] = {0};
	char c, e;
	printf("  FMT : CE Desc\n--------------------\n");
	while (0 == xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc))
	{
		strncpy(fourcc, (char *)&fmtdesc.pixelformat, 4);
		c = fmtdesc.flags & 1 ? 'C' : ' ';
		e = fmtdesc.flags & 2 ? 'E' : ' ';
		printf("  %s(%s) %c%c :\n", fourcc, fmtdesc.description, c, e);

		// get supported resolution for this format
		struct v4l2_frmsizeenum frmsizeenum = {0};
		frmsizeenum.pixel_format = fmtdesc.pixelformat;
		while (0 == xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsizeenum))
		{
			if (frmsizeenum.type == V4L2_FRMSIZE_TYPE_DISCRETE)
			{
				printf("    %d*%d: ", frmsizeenum.discrete.width, frmsizeenum.discrete.height);

				//get supported for this format and resolution
				struct v4l2_frmivalenum frmivalenum = {0};
				frmivalenum.pixel_format = fmtdesc.pixelformat;
				frmivalenum.height = frmsizeenum.discrete.height;
				frmivalenum.width = frmsizeenum.discrete.width;

				while (0 == ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmivalenum))
				{
					if (frmivalenum.type == V4L2_FRMIVAL_TYPE_DISCRETE)
					{
						printf("%d/%d", frmivalenum.discrete.numerator, frmivalenum.discrete.denominator);
					}
					else
						printf("stepwise framerate");

					frmivalenum.index++;
				}
				printf("\n");
			}
			else
				printf("stepwise framesize\n");

			frmsizeenum.index++;
		}

		fmtdesc.index++;
	}

	//get selected mode
	struct v4l2_format fmt = {0};
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
	{
		perror("Querying Pixel Format\n");
		return;
	}
	strncpy(fourcc, (char *)&fmt.fmt.pix.pixelformat, 4);
	printf("Selected Camera Mode:\n"
		   "  Width: %d\n"
		   "  Height: %d\n"
		   "  PixFmt: %s\n"
		   "  Field: %d\n",
		   fmt.fmt.pix.width,
		   fmt.fmt.pix.height,
		   fourcc,
		   fmt.fmt.pix.field);

	struct v4l2_streamparm streamparm = {0};
	streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == ioctl(fd, VIDIOC_G_PARM, &streamparm))
	{
		perror("Querying Frame Rate\n");
		return;
	}
	printf("Frame Rate:  %f\n====================\n",
		   (float)streamparm.parm.capture.timeperframe.denominator /
			   (float)streamparm.parm.capture.timeperframe.numerator);

	struct v4l2_control control_s;
	control_s.id = V4L2_CID_EXPOSURE_AUTO;
	if (-1 == xioctl(fd, VIDIOC_G_CTRL, &control_s))
	{
		perror("Querying Exposure Mode\n");
		return;
	}
	else
	{
		if (control_s.value)
			printf("Exposure Mode: Manual\n");
		else
			printf("Exposure Mode: Auto\n");
	}
	control_s.id = V4L2_CID_EXPOSURE_ABSOLUTE;
	if (-1 == xioctl(fd, VIDIOC_G_CTRL, &control_s))
	{
		perror("Querying Exposure Time");
		return;
	}
	else
	{
		printf("Exposure absolute: %f ms\n", control_s.value / 10.0);
	}
	printf("===================================================\n");

	control_s.id = V4L2_CID_ISO_SENSITIVITY_AUTO;
	if (-1 == xioctl(fd, VIDIOC_G_CTRL, &control_s))
	{
		perror("Querying ISO mode");
		return;
	}
	else
	{
		printf("ISO: %d\n", control_s.value);
	}
	printf("===================================================\n");
}

void V4LCamDriver::discardFrame()
{
	lock_guard<std::mutex> lockg(lockcam);
	FrameInfo *out = new FrameInfo(this);
	struct v4l2_buffer bufferinfo = {0};
	bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	bufferinfo.memory = V4L2_MEMORY_MMAP;
	bufferinfo.index = buffr_idx;
	if (ioctl(fd, VIDIOC_DQBUF, &bufferinfo) < 0)
	{
		perror("VIDIOC_DQBUF Error");
		exit(1);
	}
	//queue buffer back allowing the driver to read again
	bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	bufferinfo.memory = V4L2_MEMORY_MMAP;
	bufferinfo.index = buffr_idx;
	if (ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0)
	{
		perror("VIDIOC_QBUF Error");
		cout << "buffr_idx: " << buffr_idx << endl;
		exit(1);
	}
	++buffr_idx;
	buffr_idx = buffr_idx >= buffer_size ? buffr_idx - buffer_size : buffr_idx;
	++cur_frame;
};

FrameInfo *V4LCamDriver::getFrame()
{
	FrameInfo *out = new FrameInfo(this);
	//dequeue buffer
	struct v4l2_buffer bufferinfo = {0};
	bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	bufferinfo.memory = V4L2_MEMORY_MMAP;
	bufferinfo.index = buffr_idx;
	if (ioctl(fd, VIDIOC_DQBUF, &bufferinfo) < 0)
	{
		perror("VIDIOC_DQBUF Error");
		exit(1);
	}
	//decode raw data into mat
	if (!mb[buffr_idx].ptr)
		return NULL;

	cvtRaw2Mat(mb[buffr_idx].ptr, out->img);

	if (out->img.cols <= 0 || out->img.rows <= 0)
		return NULL;

	if (bufferinfo.flags & V4L2_BUF_FLAG_TSTAMP_SRC_SOE)
	{
	}

	//convert v4l2's timestamp (system time) to ros's time (wall time)
	ros::Time capTime(bufferinfo.timestamp.tv_sec,
					  bufferinfo.timestamp.tv_usec * 1000);

	out->rosheader.stamp = capTime + toEpochOffset;

	//queue a buffer back allowing the driver to read again
	bufferinfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	bufferinfo.memory = V4L2_MEMORY_MMAP;
	bufferinfo.index = buffr_idx;
	if (ioctl(fd, VIDIOC_QBUF, &bufferinfo) < 0)
	{
		perror("VIDIOC_DQBUF Error");
		exit(1);
	}
	++buffr_idx;
	buffr_idx = buffr_idx >= buffer_size ? buffr_idx - buffer_size : buffr_idx;
	++cur_frame;
	return out;
};