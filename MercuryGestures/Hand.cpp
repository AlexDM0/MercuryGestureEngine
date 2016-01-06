#pragma once
#include "MercuryCore.h"
#include "HandDetector.h"
#include <set>


// Basic constructor, initializing all variables
Hand::Hand() {
	this->position = cv::Point(0, 0);
	this->positionHistory.resize(historySize, cv::Point(0, 0));
	this->blobHistory.resize(historySize);
	this->color = CV_RGB(0, 255, 0);
}
Hand::~Hand() {}


/*
 * Set the estimated value based on the blob distribution. If this is a poor estimation, ignoreIntersection can be turned on.
 * When intersecting, the area search will prefer the estimate over the colliding result.
 */
void Hand::setEstimate(cv::Point& estimate, BlobInformation& blob, bool ignoreIntersection, Condition condition) {
	if (blob.type == HIGH || ignoreIntersection == true) {
		this->ignoreIntersect = true;
	}

	// if there is ONLY a head and the hand has not been above the threshold in the previous guess, we do not accept the blob position.
	if (condition == ONLY_HEAD) {
		if (this->position.y == 0 || this->position.y > faceCoverageThreshold) {
			return;
		}
	}

	this->blobEstimate = estimate;
	this->blobIndex = this->getNextIndex(this->blobIndex);
	this->blobHistory[this->blobIndex] = blob;
	this->estimateUpdated = true;
}


/*
* Draw the hand marker on the canvas
*/
void Hand::draw(cv::Mat& canvas) {
	if (this->position.y == 0) {
		cv::putText(canvas, this->leftHand ? "Left hand missing." : "Right hand missing.", this->leftHand ? cv::Point(20,30) : cv::Point(20, 60), 0, 1, this->color, 2);
	}
	else {
		cv::circle(canvas, this->position, 25, this->color, 2);
		cv::circle(*this->rgbSkinMask, this->position, 25, this->color, 2);
		cv::putText(canvas, this->leftHand ? "L" : "R", this->position, 0, 0.8, this->color, 3);

#ifdef DEBUG
		if (this->leftHand)
			this->drawTrace(canvas, this->positionHistory, this->positionIndex, 0, 150, 255);
		else
			this->drawTrace(canvas, this->positionHistory, this->positionIndex, 0, 255, 0);
	}
#endif
}

void Hand::drawTrace(cv::Mat& canvas, std::vector<cv::Point>& positions, int startIndex, int r, int g, int b) {
	int traceSize = positions.size();

	// since the history is a deck and we want to draw from oldest to newest, we index forward instead of backward.
	int index = (startIndex + 1) % traceSize;

	// get step colors 
	double colorSteps = double(traceSize);
	double rStep = r / colorSteps;
	double gStep = g / colorSteps;
	double bStep = b / colorSteps;
	double m = 255 / colorSteps;

	// mask for nice drawing (fade out effects)
	cv::Mat traceMap = cv::Mat::zeros(canvas.rows, canvas.cols, canvas.type());
	cv::Mat traceMask = cv::Mat::zeros(canvas.rows, canvas.cols, canvas.type());	

	// for n steps, we need n-1 lines.
	for (int i = 0; i < traceSize - 1; i++) {
		cv::Point p1 = positions[index];
		index = (index + 1) % traceSize;
		cv::Point p2 = positions[index];
		if (p1.x != 0 && p1.y != 0 && p2.x != 0 && p2.y != 0) {
			cv::line(traceMap, p1, p2, CV_RGB(i * rStep, i * gStep, i * bStep), 2, cv::LINE_AA);
			cv::line(traceMask, p1, p2, CV_RGB(m*i, m*i, m*i), 2, cv::LINE_AA);
		}
	}

	// apply mask and add line blended in
	cv::subtract(canvas, traceMask, canvas);
	cv::addWeighted(canvas, 1, traceMap, 1, 0, canvas);
}

/*
* Based on all input this iteration, we will try to find the best estimate for the hand position.
* We do this by 
	- looking from the last position for the blob in the area search. 
	- If this fails we try a predicted position based on the velocity of the blob.
    - Refine the result by area optimalization and transversing the blob.
	- Refine the result by averaging over time, if matched we will use this to smooth out the movement.
*
*/
void Hand::solve(cv::Mat& skinMask, std::vector<BlobInformation>& blobs, cv::Mat& movementMap) {
	//std::cout << (this->leftHand ? "LEFT HAND:" : "RIGHT HAND:") << "--------------- solve -----------" << std::endl;
	// if the estimate has been updated, update the position. If the improvement algorithms fail, this is the fallback
	if (this->estimateUpdated == true) {
		this->position = this->blobEstimate;
	}
	
	// search for a position based on the last known position
	auto lastPosition = this->positionHistory[this->positionIndex]; // still the last one since we have not yet found the final pos.
	if (lastPosition.x != 0 && lastPosition.y != 0 && this->intersecting == false) {
		bool areaSearched = this->improveByAreaSearch(skinMask, lastPosition);
		// if we did not search because of fast moving objects, we try again with a predicted position.
		if (!areaSearched) {
			auto predictedPoint = this->getPredictedPosition(skinMask);
			// if we have a predicted position, use that.
			if (predictedPoint.x != 0 && predictedPoint.y != 0) {
				this->improveByAreaSearch(skinMask, predictedPoint);
			}
		}
	}

	
	// we do not want to improve the position if it is not initialized.
	if (this->position.x != 0 && this->position.y != 0) {
		// get a search mode based on the location of the blob
		SearchMode searchMode = this->getSearchModeFromBlobs(blobs);

		// find a good estimate
		this->improveByCoverage(skinMask, searchMode, 5);

		// check if we can use averaging to smooth the result.
		this->improveUsingHistory(skinMask, movementMap);

		// store the position in the list
		this->positionIndex = this->getNextIndex(this->positionIndex);
		this->positionHistory[this->positionIndex] = this->position;

		// spline fit position history
		this->updateLastPoint();
	}

	// reset to clean state
	this->estimateUpdated = false;
}


void Hand::updateLastPoint() {
	int p0_index = this->positionIndex;
	int p1_index = this->getPreviousIndex(p0_index);
	int p2_index = this->getPreviousIndex(p1_index);
	
	// we do not use this esitmate if the points have not yet been started.
	if (this->positionHistory[p2_index].x == 0 && this->positionHistory[p2_index].y == 0) {
		return;
	}

	this->positionHistory[p1_index].x = 0.5 * (this->positionHistory[p0_index].x + this->positionHistory[p2_index].x);
	this->positionHistory[p1_index].y = 0.5 * (this->positionHistory[p0_index].y + this->positionHistory[p2_index].y);
}

/*
* This method will search the surrounding 8.5 cm for a hand blob.
*/
bool Hand::improveByAreaSearch(cv::Mat& skinMask, cv::Point& position) {
	double distance = getDistance(position, this->position);
	double maxDistance = 2 * this->maxVelocity * cmInPixels / fps;

	// if a jump or if no data
	if (distance > maxDistance || this->estimateUpdated == false) {
		int maxIterations = 10;
		int stepSize = 4;
		int radius = 8.5 * this->cmInPixels;

		double pointQuality = this->getPointQuality(position, skinMask);
#ifdef DEBUG
		cv::circle(*this->rgbSkinMask, position, 5*this->cmInPixels, CV_RGB(0, 100, 30), 4);
		cv::putText(*this->rgbSkinMask, joinString("q:", int(100 * pointQuality)), position + cv::Point(10, 0), 0, 1, CV_RGB(0, 100, 30), 2);
#endif
		// We do a quality check to ensure that the point we are in is not crap. 
		// If it is we need to ignore the search and get the estimate.
		if (pointQuality > 0.2) {
			this->position = this->lookAround(position, skinMask, maxIterations, stepSize, radius, FREE_SEARCH, 50);
			return true;
		}
	}
	return false;
}

/*
* We get a position based on a linear extrapolation from the last point. 
*/
cv::Point Hand::getPredictedPosition(cv::Mat& skinMask) {
	int p1_index = this->positionIndex;
	int p2_index = this->getPreviousIndex(p1_index);
	int p3_index = this->getPreviousIndex(p2_index);

	auto position_n_1 = this->positionHistory[p1_index]; // still the last one since we have not yet found the final pos.
	auto position_n_2 = this->positionHistory[p2_index];
	auto position_n_3 = this->positionHistory[p3_index];

	int dx1 = position_n_1.x - position_n_2.x;
	int dy1 = position_n_1.y - position_n_2.y;

	int dx2 = position_n_2.x - position_n_3.x;
	int dy2 = position_n_2.y - position_n_3.y;

	cv::Point predictedPosition(		position_n_1.x + dx1,			  position_n_1.y + dy1);
	cv::Point predictedPositionVelocity(position_n_1.x + (dx1 + dx2) / 2, position_n_1.y + (dy1 + dy2) / 2);

	double pointQuality			= this->getPointQuality(predictedPosition, skinMask);
	double pointQualityVelocity = this->getPointQuality(predictedPositionVelocity, skinMask);

	if (pointQualityVelocity > pointQuality) {
		predictedPosition = predictedPositionVelocity;
	}


	// if about 20% of the searchspace has been filled we accept the point
	if (pointQuality > 0.2) {
#ifdef DEBUG
		rect(*this->rgbSkinMask, predictedPosition, 10, CV_RGB(255, 0, 0), 5); // orange rect
		cv::putText(*this->rgbSkinMask, joinString("q:", int(100 * pointQuality)), predictedPosition + cv::Point(10, 0), 0, 1, CV_RGB(255, 0, 10), 2);
		rect(*this->rgbSkinMask, predictedPositionVelocity, 10, CV_RGB(0, 255, 0), 5); // orange rect
		cv::putText(*this->rgbSkinMask, joinString("q:", int(100 * pointQualityVelocity)), predictedPositionVelocity + cv::Point(10, 0), 0, 1, CV_RGB(0, 255, 0), 2);
#endif
		return predictedPosition;
	}
	else {
#ifdef DEBUG
		rect(*this->rgbSkinMask, predictedPosition, 60, CV_RGB(255, 0, 0), 5); // orange rect
		rect(*this->rgbSkinMask, predictedPosition, 10, CV_RGB(255, 0, 0), 5); // orange rect
		cv::putText(*this->rgbSkinMask, joinString("q:", int(100 * pointQuality)), predictedPosition + cv::Point(10, 0), 0, 1, CV_RGB(255, 0, 10), 2);
		rect(*this->rgbSkinMask, predictedPositionVelocity, 10, CV_RGB(0, 255, 0), 5); // orange rect
		cv::putText(*this->rgbSkinMask, joinString("q:", int(100 * pointQualityVelocity)), predictedPositionVelocity + cv::Point(10, 0), 0, 1, CV_RGB(0, 255, 0), 2);
#endif
	}

	// if the new position is bad, we return an empty point
	return cv::Point(0, 0);
}



/*
* Based on where the blob is, we can search the space differently in order to find the hand position on the blob.
*/
SearchMode Hand::getSearchModeFromBlobs(std::vector<BlobInformation>& blobs) {
	for (int i = 0; i < blobs.size(); i++) {
		// determine which blob contains our point (bounding box)
		if (blobs[i].left.x   <= this->position.x &&
			blobs[i].right.x  >= this->position.x && 
			blobs[i].top.y    <= this->position.y &&
			blobs[i].bottom.y >= this->position.y) {

			int height = (blobs[i].bottom.y - blobs[i].top.y);

			if (height < 15 * this->cmInPixels) { // height less than 15 cm --> normal search
				return FREE_SEARCH;
			}
			else {
				if (blobs[i].type == LOW) {
					return SEARCH_DOWN;
				}
				else if (blobs[i].type == MEDIUM) {
					if (height > 40 * this->cmInPixels)
						return SEARCH_DOWN;
					else 
						return FREE_SEARCH;
				}
				else if (blobs[i].type == HIGH) {
					return SEARCH_UP;
				}
				else {
					return FREE_SEARCH;
				}
			}
		}
	}
	return FREE_SEARCH;
}


/*
* We use a small tracker to walk over the blob, 
* hoping to optimize the area and to move over long blobs towards the likely position of the hand.
*/
void Hand::improveByCoverage(cv::Mat& skinMask, SearchMode searchMode, int maxIterations, int colorBase) {
	int stepSize = 3;
	int radius = 5 * this->cmInPixels; 

	// find the new best position
	cv::Point maxPos = this->lookAround(this->position, skinMask, maxIterations, stepSize, radius, searchMode, colorBase);

	// update position with improved one.
	this->position = maxPos;
}

/*
* if there is no movement we use the average position.
* if there is a little movement, we weight he current position more strongly.
* if there is a lot of movement, we accept the current position.
*/
void Hand::improveUsingHistory(cv::Mat& skinMask, cv::Mat& movementMap) {
	int historyAverage = 5; // must be lower or equal to this->historySize
	int index = this->positionIndex; 
	double distanceThreshold = 3 * this->cmInPixels;

	double avgX = 0;
	double avgY = 0;
	for (int i = 0; i < historyAverage; i++) {
		// we do not average if some of the points are not filled
		if (this->positionHistory[index].x == 0 && this->positionHistory[index].y == 0) {
			return;
		}
		avgX += this->positionHistory[index].x;
		avgY += this->positionHistory[index].y;
		index = this->getPreviousIndex(index);
	}
	avgX /= historyAverage;
	avgY /= historyAverage;

	double movementCoverage = this->getPointQuality(this->position, movementMap, 30);
	
	// if the movement is very small, mostly copy over the average
	if (movementCoverage < 0.001) {
		this->position.x = 0.95 * avgX + 0.05 * this->position.x;
		this->position.y = 0.95 * avgY + 0.05 * this->position.y;
	}
	// if there is some movement, average the average and the pos by 80/20
	else if (movementCoverage < 0.05) {
		//rect(*this->rgbSkinMask, cv::Point(avgX, avgY), 10, CV_RGB(200, 0, 200), 8);
		this->position.x = 0.8 * avgX + 0.2 * this->position.x;
		this->position.y = 0.8 * avgY + 0.2 * this->position.y;
	}
	// if there is reasonable movement, average the average and the pos by 50/50
	else if (movementCoverage < 0.2) {
		//rect(*this->rgbSkinMask, cv::Point(avgX, avgY), 15, CV_RGB(0, 200, 200), 8);
		this->position.x = 0.5 * avgX + 0.5 * this->position.x;
		this->position.y = 0.5 * avgY + 0.5 * this->position.y;
	}
	// if there is more, use the position and ignore the average
}


/*
* This method will check if there is an intersection with the "other" hand. If so, move it to the side to total overlap is not attained.
*/
void Hand::handleIntersection(cv::Point otherHandPosition, cv::Mat& skinMask) {
	// uninitialized intersections are not relevant.
	if (this->position.x != 0 && this->position.y != 0 && otherHandPosition.x != 0 && otherHandPosition.y != 0) {
		this->intersecting = false;
		int minimalDistance = 8 * this->cmInPixels;
		int dx = std::abs(this->position.x - otherHandPosition.x);
		int dy = std::abs(this->position.y - otherHandPosition.y);

		double distance = std::max(1.0, std::sqrt(dx*dx + dy*dy));

		// if we're close we search for space away from the other hand and set intersecting to true
		if (distance < minimalDistance) {
#ifdef DEBUG
			rect(*this->rgbSkinMask, this->position, 40, CV_RGB(10, 40, 255), 3);
			cv::putText(*this->rgbSkinMask, "intersect ON, forcibly", this->position - cv::Point(80, 40), 0, 0.5, this->color);
#endif
			this->intersecting = true;
			this->improveByCoverage(skinMask, this->leftHand ? SEARCH_LEFT : SEARCH_RIGHT, 20, 100);
		}
		// if we're only a little close, push a way gently
		else if (distance < 2 * minimalDistance) {
#ifdef DEBUG
			rect(*this->rgbSkinMask, this->position, 40, CV_RGB(10, 40, 255), 1);
			cv::putText(*this->rgbSkinMask, "intersect detected, gently", this->position - cv::Point(100, 40), 0, 0.5, this->color);
#endif
			this->improveByCoverage(skinMask, this->leftHand ? SEARCH_LEFT : SEARCH_RIGHT, 5, 150);
		}

		if (this->ignoreIntersect) {
			this->intersecting = false;
		}
		this->ignoreIntersect = false;
	}
}



//***************************************** PRIVATE  **********************************************//


/*
* Explore the area around the blob for maximum coverage. This will center a circle within the blob (ideally).
*/
cv::Point Hand::lookAround(cv::Point start, cv::Mat& skinMask, int maxIterations,int stepSize, int radius, SearchMode searchMode, int colorBase) {
	cv::Point maxPos = start;

	SearchSpace space;
	getSearchSpace(space, skinMask, maxPos);

#ifdef DEBUG
	cv::circle(*this->rgbSkinMask, maxPos, radius, this->color, 1);
	//cv::circle(*this->rgbSkinMask, maxPos, 3, CV_RGB(0, 80, 180), 5);
#endif

	// Offet the position by the searchwindow
	toSearchSpace(space, maxPos);

	// get the initial estimate.
	double maxValue = this->getCoverage(maxPos, space.mat, radius);

	// search in a box
	int index_n_1 = -1;
	int index_n_2 = -1;

	int vectorSize = 5;
	if (searchMode == FREE_SEARCH) {
		vectorSize = 8;
	}
	std::vector<double> newValues(vectorSize,0);
	std::vector<cv::Point> newPositions(vectorSize, cv::Point(0,0));
	
	std::set<int> positionHistory;
	for (int i = 0; i < maxIterations; i++) {

		// WHEN SEARCH_RIGHT IS ON, WE SEARCH ON THE LEFT SIDE OF THE SCREEN -> RIGHT FOR THE PERSON
		if (searchMode == SEARCH_RIGHT) {
			// also search up and down, just not right
			newValues[0] = this->shiftPosition(space.mat, newPositions[0], maxPos, 0, stepSize, radius);
			newValues[1] = this->shiftPosition(space.mat, newPositions[1], maxPos, 0, -stepSize, radius);
			newValues[2] = this->shiftPosition(space.mat, newPositions[2], maxPos, -stepSize, stepSize, radius);
			newValues[3] = this->shiftPosition(space.mat, newPositions[3], maxPos, -stepSize, -stepSize, radius);
			newValues[4] = this->shiftPosition(space.mat, newPositions[4], maxPos, -stepSize, 0, radius);
		}
		// WHEN SEARCH_LEFT IS ON, WE SEARCH ON THE RIGHT SIDE OF THE SCREEN -> LEFT FOR THE PERSON
		else if (searchMode == SEARCH_LEFT) {
			// also search up and down, just not left
			newValues[0] = this->shiftPosition(space.mat, newPositions[0], maxPos, 0, stepSize, radius);
			newValues[1] = this->shiftPosition(space.mat, newPositions[1], maxPos, 0, -stepSize, radius);
			newValues[2] = this->shiftPosition(space.mat, newPositions[2], maxPos, stepSize, stepSize, radius);
			newValues[3] = this->shiftPosition(space.mat, newPositions[3], maxPos, stepSize, -stepSize, radius);
			newValues[4] = this->shiftPosition(space.mat, newPositions[4], maxPos, stepSize, 0, radius);
		}
		else if (searchMode == SEARCH_UP) {
			// allow left right
			newValues[0] = this->shiftPosition(space.mat, newPositions[0], maxPos, -stepSize, 0, radius);
			newValues[1] = this->shiftPosition(space.mat, newPositions[1], maxPos, stepSize, 0, radius);
			newValues[2] = this->shiftPosition(space.mat, newPositions[2], maxPos, stepSize, -stepSize, radius);
			newValues[3] = this->shiftPosition(space.mat, newPositions[3], maxPos, -stepSize, -stepSize, radius);
			newValues[4] = this->shiftPosition(space.mat, newPositions[4], maxPos, 0, -stepSize, radius);
		}
		else if (searchMode == SEARCH_DOWN) {
			// allow left right
			newValues[0] = this->shiftPosition(space.mat, newPositions[0], maxPos, -stepSize, 0, radius);
			newValues[1] = this->shiftPosition(space.mat, newPositions[1], maxPos, stepSize, 0, radius);
			newValues[2] = this->shiftPosition(space.mat, newPositions[2], maxPos, stepSize, stepSize, radius);
			newValues[3] = this->shiftPosition(space.mat, newPositions[3], maxPos, -stepSize, stepSize, radius);
			newValues[4] = this->shiftPosition(space.mat, newPositions[4], maxPos, 0, stepSize, radius);
		}
		else {// if (searchMode == FREE_SEARCH) {
			newValues[0] = this->shiftPosition(space.mat, newPositions[0], maxPos, stepSize, stepSize, radius);
			newValues[1] = this->shiftPosition(space.mat, newPositions[1], maxPos, stepSize, -stepSize, radius);
			newValues[2] = this->shiftPosition(space.mat, newPositions[2], maxPos, stepSize, 0, radius);
			newValues[3] = this->shiftPosition(space.mat, newPositions[3], maxPos, -stepSize, stepSize, radius);
			newValues[4] = this->shiftPosition(space.mat, newPositions[4], maxPos, -stepSize, -stepSize, radius);
			newValues[5] = this->shiftPosition(space.mat, newPositions[5], maxPos, -stepSize, 0, radius);
			newValues[6] = this->shiftPosition(space.mat, newPositions[6], maxPos, 0, stepSize, radius);
			newValues[7] = this->shiftPosition(space.mat, newPositions[7], maxPos, 0, -stepSize, radius);
		}

	
		double newMax = 0;
		int maxIndex = 0;
		for (int j = 0; j < vectorSize; j++) {
			if (newMax < newValues[j]) {
				// if position not in the positions, allow.
				if (positionHistory.find(newPositions[j].x * 1000 + newPositions[j].y) == positionHistory.end()) {
					newMax = newValues[j];
					maxIndex = j;
				}
			}
		}

		if (newMax >= maxValue) {
			maxValue = newMax;
#ifdef DEBUG
			if (this->leftHand) {
				cv::Point drawPoint(maxPos.x + space.x, maxPos.y + space.y);
				cv::circle(*this->rgbSkinMask, drawPoint, 2, CV_RGB(colorBase, 0, std::min(30 * i, 255)), 5);
			}
#endif
			maxPos = newPositions[maxIndex];
			positionHistory.insert(maxPos.x * 1000 + maxPos.y);
		}
		else {
			break;
		}
	}

	// restore the transformation of the coordinates
	fromSearchSpace(space, maxPos);

	auto color = this->color;
	if (colorBase < 150)
		color = CV_RGB(100, 0, 100);
	if (searchMode == SEARCH_RIGHT) {
		cv::line(*this->rgbSkinMask, maxPos, cv::Point(maxPos.x - 40, maxPos.y), color, 2);
		cv::circle(*this->rgbSkinMask, cv::Point(maxPos.x - 40, maxPos.y), 10, color, 2);
	}
	// WHEN SEARCH_LEFT IS ON, WE SEARCH ON THE RIGHT SIDE OF THE SCREEN -> LEFT FOR THE PERSON
	else if (searchMode == SEARCH_LEFT) {
		cv::line(*this->rgbSkinMask, maxPos, cv::Point(maxPos.x + 40, maxPos.y), color, 2);
		cv::circle(*this->rgbSkinMask, cv::Point(maxPos.x + 40, maxPos.y), 10, color, 2);
	}
	else if (searchMode == SEARCH_UP) {
		cv::line(*this->rgbSkinMask, maxPos, cv::Point(maxPos.x, maxPos.y - 40), color, 2);
		cv::circle(*this->rgbSkinMask, cv::Point(maxPos.x, maxPos.y - 40), 10, color, 2);
	}
	else if (searchMode == SEARCH_DOWN) {
		cv::line(*this->rgbSkinMask, maxPos, cv::Point(maxPos.x, maxPos.y + 40), color, 2);
		cv::circle(*this->rgbSkinMask, cv::Point(maxPos.x, maxPos.y + 40), 10, color, 2);
	}
	else { // searchMode == FREE_SEARCH
		cv::line(*this->rgbSkinMask, cv::Point(maxPos.x, maxPos.y - 40), cv::Point(maxPos.x, maxPos.y + 40), color, 2);
		cv::line(*this->rgbSkinMask, cv::Point(maxPos.x - 40, maxPos.y), cv::Point(maxPos.x + 40, maxPos.y), color, 2);
		cv::circle(*this->rgbSkinMask, maxPos, 10, color, 2);
	}

	// return best position
	return maxPos;
}


/*
* We use a circle mask and get a percentage of how much this was filled by the blob. 
* Returns value between 0 .. 1
*/
double Hand::getCoverage(cv::Point& pos, cv::Mat& blobMap, int radius) {
	cv::Mat mask = cv::Mat::zeros(blobMap.rows, blobMap.cols, blobMap.type()); // all 0
	cv::circle(mask, pos, radius, 255, CV_FILLED);
	cv::Mat result;
	cv::bitwise_and(blobMap, mask, result);
	
	double value = cv::sum(result)[0] / (radius*radius*3.1415 * 255.0);
	return value;
}


/*
* We shift the point in the x and y direction and get the coverage
*/
double Hand::shiftPosition(
	cv::Mat& skinMask, cv::Point& newPosition,
	cv::Point& basePosition, int xOffset, int yOffset, int radius) {

	cv::Point newPos = basePosition;
	newPos.x += xOffset;
	newPos.y += yOffset;
	newPosition = newPos;
	return this->getCoverage(newPos, skinMask, radius);
}

// Small util to get the quality with a default radius... should be removed..
double Hand::getPointQuality(cv::Point& point, cv::Mat& qualityMask, int radius) {
	if (radius == 0)
		radius = 5 * this->cmInPixels; // cm

	SearchSpace space;
	getSearchSpace(space, qualityMask, point, 2 * radius);
	toSearchSpace(space,point);
	double quality = this->getCoverage(point, space.mat, radius);
	fromSearchSpace(space, point);

	//cv::imshow("point", space.mat);

	return quality;
}

// History is a deck. Get the index:
int Hand::getNextIndex(int index) {
	return (index + 1) % this->historySize;
}

// History is a deck. Get the index:
int Hand::getPreviousIndex(int index) {
	return (index - 1) < 0 ? index - 1 + this->historySize : index - 1;
}

