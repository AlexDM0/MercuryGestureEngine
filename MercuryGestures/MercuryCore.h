#pragma once

#include "opencv2/opencv.hpp"
#include <iostream>
#include <chrono>
#include <ctime>
#include <stdio.h>

// based on // https://upload.wikimedia.org/wikipedia/commons/6/61/HeadAnthropometry.JPG
const double averageFaceWidth = 15.705; //cm (95th percentile)
const double averageFaceHeight = 23.95; //cm (95th percentile)
const double faceWidthToHeightRatio = averageFaceWidth / averageFaceHeight;

struct FaceData {
	cv::Rect rect;
	int count;
};

struct BodyRects {
	cv::Rect face;
	cv::Rect upperTorso;
	cv::Rect lowerTorso;
	cv::Rect lap;
	cv::Rect armRightUpper;
	cv::Rect armRightLower;
	cv::Rect armLeftUpper;
	cv::Rect armLeftLower;
};

/*
 * Get center x and y for a rect
 */
int getCenterX(cv::Rect& face);
int getCenterY(cv::Rect& face);

/*
 * return a value bounded between 0 and 255.
 */
int rgbBound(int color);

/**
 * get the average value of a vector.
 */
double getAverage(std::vector<double> data);

/* 
 * Join two strings together.
 */
std::string joinString(std::string a, std::string b);
std::string joinString(std::string a, int b);

/**
 * this method is a preconfigured dilate call
 */
void dilate(cv::Mat& inputFrame, cv::Mat& outputFrame, int kernelSize = 6);

/**
 * this method is a preconfigured erode call
 */
void erode(cv::Mat& inputFrame, cv::Mat& outputFrame, int kernelSize = 6);

/**
 * remove noise by dilating first, then eroding.
 */
void dilateErodeNoiseRemoval(cv::Mat& inputFrame, cv::Mat& outputFrame, int kernelSize = 6);

/**
 * this method uses calculates the length of the contours of the blobs in the input frame and uses those to remove noise.
 */
void contourNoise(cv::Mat& inputFrameWithBlobs, cv::Mat& outputFrame, int contourThreshold);
