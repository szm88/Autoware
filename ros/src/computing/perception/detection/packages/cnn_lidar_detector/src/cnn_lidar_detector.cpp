/*
 *  Copyright (c) 2017, Nagoya University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name of Autoware nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "cnn_lidar_detector.hpp"

void CnnLidarDetector::Detect(const cv::Mat& in_depth_image, const cv::Mat& in_height_image, cv::Mat& out_objectness_image)
{
	caffe::Blob<float>* input_layer = net_->input_blobs()[0];
	input_layer->Reshape(1,
						num_channels_,
						input_geometry_.height,
						input_geometry_.width);
	/* Forward dimension change to all layers. */
	net_->Reshape();

	std::vector<cv::Mat> input_channels;
	WrapInputLayer(&input_channels);//create pointers for input layers

	PreProcess(in_depth_image, in_height_image, &input_channels);

	net_->Forward();

	GetNetworkResults(out_objectness_image);

}

void CnnLidarDetector::GetNetworkResults(cv::Mat& out_objectness_image)
{
	caffe::Blob<float>* boxes_blob = net_->output_blobs().at(0);//0 boxes
	caffe::Blob<float>* objectness_blob = net_->output_blobs().at(1);//1 objectness

	//output layer     0  1    2     3
	//prob 		shape  1 04 height width
	//bb_score 	shape  1 24 height width
	CHECK_EQ(boxes_blob->shape(1), 24) << "The output bb_score layer should be 24 channel image, but instead is " << boxes_blob->shape(1);
	CHECK_EQ(objectness_blob->shape(1), 4) << "The output prob layer should be 4 channel image, but instead is " << objectness_blob->shape(1) ;

	std::vector<cv::Mat> objectness_channels;
	int width = objectness_blob->shape(3);
	int height = objectness_blob->shape(2);
	float* objectness_ptr = objectness_blob->mutable_cpu_data();//pointer to the prob layer
	//copy each channel(class) from the output layer to a Mat
	for (int i = 0; i < objectness_blob->shape(1); ++i)
	{
		cv::Mat channel(height, width, CV_32FC1, objectness_ptr);
		cv::normalize(channel, channel, 1, 0, cv::NORM_MINMAX);
		objectness_channels.push_back(channel);
		objectness_ptr += width * height;
	}

	//cv::imshow("Channel 1", objectness_channels[1]);
	//cv::imshow("Channel 2", objectness_channels[2]);
	//cv::imshow("Channel 3", objectness_channels[3]);
	//cv::waitKey(10);

	//check each pixel of each channel and assign color depending threshold
	cv::Mat bgr_channels(height, width, CV_8UC3, cv::Scalar(0,0,0));

	for(unsigned int h = 0; h < height; h++)
	{
		for(unsigned int w = 0; w < width; w++)
		{
			//0 nothing
			//1 car, red
			//2 person, green
			//3 bike, blue
			//BGR Image
			if (
					objectness_channels[1].at<float>(h,w) > score_threshold_
				)
				bgr_channels.at<cv::Vec3b>(h,w) = cv::Vec3b(0, 0, 255);
			if (objectness_channels[2].at<float>(h,w) > score_threshold_
				)
				bgr_channels.at<cv::Vec3b>(h,w) = cv::Vec3b(0, 255, 0);
			if (objectness_channels[3].at<float>(h,w) > score_threshold_
				)
				bgr_channels.at<cv::Vec3b>(h,w) = cv::Vec3b(255, 0, 0);
		}
	}

	cv::flip(bgr_channels, out_objectness_image,-1);
}

void CnnLidarDetector::PreProcess(const cv::Mat& in_depth_image, const cv::Mat& in_height_image, std::vector<cv::Mat>* in_out_channels)
{
	//resize image if required
	cv::Mat depth_resized;
	cv::Mat height_resized;

	if (in_depth_image.size() != input_geometry_)
		cv::resize(in_depth_image, depth_resized, input_geometry_);
	else
		depth_resized = in_depth_image;

	if (in_height_image.size() != input_geometry_)
			cv::resize(in_height_image, height_resized, input_geometry_);
		else
			height_resized = in_height_image;

	//depth and height images are already preprocessed
	//put each corrected mat geometry onto the correct input layer type pointers
	depth_resized.copyTo(in_out_channels->at(0));
	height_resized.copyTo(in_out_channels->at(1));

	//check that the pre processed and resized mat pointers correspond to the pointers of the input layers
	CHECK(reinterpret_cast<float*>(in_out_channels->at(0).data) == net_->input_blobs()[0]->cpu_data())	<< "Input channels are not wrapping the input layer of the network.";

}

void CnnLidarDetector::WrapInputLayer(std::vector<cv::Mat>* in_out_channels)
{
	caffe::Blob<float>* input_layer = net_->input_blobs()[0];

	int width = input_layer->width();
	int height = input_layer->height();
	float* input_data = input_layer->mutable_cpu_data();
	for (int i = 0; i < input_layer->channels(); ++i)
	{
		cv::Mat channel(height, width, CV_32FC1, input_data);
		in_out_channels->push_back(channel);
		input_data += width * height;
	}
}

CnnLidarDetector::CnnLidarDetector(const std::string& in_network_definition_file,
		const std::string& in_pre_trained_model_file,
		bool in_use_gpu,
		unsigned int in_gpu_id,
		float in_score_threshold)
{
	if(in_use_gpu)
	{
		caffe::Caffe::set_mode(caffe::Caffe::GPU);
		caffe::Caffe::SetDevice(in_gpu_id);
	}
	else
		caffe::Caffe::set_mode(caffe::Caffe::CPU);

	/* Load the network. */
	net_.reset(new caffe::Net<float>(in_network_definition_file, caffe::TEST));
	net_->CopyTrainedLayersFrom(in_pre_trained_model_file);

	caffe::Blob<float>* input_layer = net_->input_blobs()[0];

	num_channels_ = input_layer->channels();

	input_geometry_ = cv::Size(input_layer->width(), input_layer->height());

	score_threshold_ = in_score_threshold;

}
