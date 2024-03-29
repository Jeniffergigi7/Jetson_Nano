/*
 * Copyright (c) 2017, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <gstCamera.h>

#include "glDisplay.h"
#include "cudaFont.h"

#include "commandLine.h"
#include "cudaMappedMemory.h"

#include "loadImage.h"

#include "segNet.h"
#include "imageNet.h"

#include <signal.h>

#define ABS_LOC "/home/nvidia/images/img%.3d.jpg"

/**
 * Patrick Henz
**/
extern "C" {
#include "../include/Messages.h"
#include "../include/SharedMem.h"
}

int main( int argc, char** argv )
{
	fd_set rdfs;
	int i;
	int masterRead;
	int masterWrite;
	int camWidth, camHeight;
	int readFds[1];
	int imagesTaken;
	int killMessageReceived;
	SharedMem * sharedMem;
	Message message;

	if (argc != 3)
	{
		printf("Error starting Camera Node\n");
		return -1;
	}	

	imagesTaken = 0;

	masterRead = atoi(argv[1]);
	masterWrite = atoi(argv[2]);

	readFds[0] = masterRead;

	SetupSetAndWait(readFds,1);

	/*
	 * create the camera device
	 */
	gstCamera* camera = gstCamera::Create(gstCamera::DefaultWidth,
					      gstCamera::DefaultHeight,
					      NULL);

	if( !camera )
	{
		printf("\nsegnet-camera:  failed to initialize camera device\n");
		return 0;
	}
	
	printf("\nsegnet-camera:  successfully initialized camera device\n");
	//printf("    width:  %u\n", camera->GetWidth());
	//printf("   height:  %u\n", camera->GetHeight());
	//printf("    depth:  %u (bpp)\n\n", camera->GetPixelDepth());
	camWidth = camera->GetWidth();
	camHeight = camera->GetHeight();

	sharedMem = CreateSharedMemory((camWidth * camHeight), SegmentationData);

	if (NULL == sharedMem) {
		printf("SHARED MEMORY ERROR IN CAM NODE\n");
		pause();
	}

	memset(&message, 0, sizeof(message));

	message.messageType = SharedMemory;
	message.shMem.width = camWidth;
	message.shMem.height = camHeight;
	message.source = TX2Cam;
	message.destination = TX2Nav;

	sleep(5);

	// signal nav node that shared mem is ready
	write(masterWrite, &message, sizeof(message));

	/*
	 * create segmentation network
	 */
	segNet* sNet = segNet::Create(segNet::FCN_ALEXNET_CITYSCAPES_HD);
	
	if( !sNet )
	{
		printf("segnet-camera:   failed to initialize segNet\n");
		return 0;
	}

	/*
	 * create recognition network
	 */
	imageNet* iNet = imageNet::Create(argc, argv);

	if( !iNet )
	{
		printf("imagenet-camera: failed to initialize imageNet\n");
		return 0;
	}

	// can we remove this?
	// set alpha blending value for classes that don't explicitly already have an alpha	
	sNet->SetGlobalAlpha(120);

	// allocate segmentation overlay output buffer
	float* outCPU  = NULL;
	float* outCUDA = NULL;

	if( !cudaAllocMapped((void**)&outCPU, (void**)&outCUDA, camera->GetWidth() * camera->GetHeight() * sizeof(float) * 4) )
	{
		printf("segnet-camera:  failed to allocate CUDA memory for output image (%ux%u)\n", camera->GetWidth(), camera->GetHeight());
		return 0;
	}

	//  produces overlay
	cudaFont* font = cudaFont::Create();

	/*
	 * start streaming
	 */
	if( !camera->Open() )
	{
		printf("segnet-camera:  failed to open camera for streaming\n");
		return 0;
	}
	
	printf("segnet-camera:  camera open for streaming\n");
	
	
	/*
	 * processing loop
	 */
	float confidence = 0.0f;

	printf("\n\nINITIALIZATION OF TX2 CAMERA NODE COMPLETE\n\n");
	
	// Patrick Henz
	uint8_t * mask = (uint8_t *)(sharedMem + 1);

	killMessageReceived = 0;

	while(!killMessageReceived)
	{
		if (SetAndWait(&rdfs, 1, 0) < 0 ) {
			printf("SET AND WAIT ERROR CAM\n");
		}

		for (i = 0; i < 1; i++) {
			if (!FD_ISSET(readFds[i], &rdfs)) {
				continue;
			}

			read(readFds[i], &message, sizeof(message));

			if (message.messageType == CamMessage)
			{

				// capture RGBA image
				float* imgRGBA = NULL;
				
				if( !camera->CaptureRGBA(&imgRGBA, 1000, true) )
					printf("segnet-camera:  failed to convert from NV12 to RGBA\n");
				
				// classify image
				const int img_class = iNet->Classify(imgRGBA, camWidth, camHeight, &confidence);
	
				if( img_class >= 0 )
				{
					printf("imagenet-camera:  %2.5f%% class #%i (%s)\n", confidence * 100.0f, img_class, iNet->GetClassDesc(img_class));	

					if( font != NULL )
					{
						char str[256];
						sprintf(str, "%05.2f%% %s", confidence * 100.0f, iNet->GetClassDesc(img_class));
	
						font->OverlayText((float4*)imgRGBA, camWidth, camHeight,
								        str, 5, 5, make_float4(255, 255, 255, 255), make_float4(0, 0, 0, 100));
					}
				}

			        CUDA(cudaDeviceSynchronize());	

				memset(&message, 0, sizeof(message));
				sprintf(message.camMsg.fileLocation, ABS_LOC, imagesTaken);
				saveImageRGBA(message.camMsg.fileLocation, (float4*)imgRGBA, camWidth, camHeight);

			        CUDA(cudaDeviceSynchronize());	
				imagesTaken++;

				message.messageType = CamMessage;
				message.source = TX2Cam;
				message.destination = TX2Comm;

				write(masterWrite, &message, sizeof(message));
			}
			else if (message.messageType == SharedMemory)
			{
				float * imgRGBA = NULL;

				if( !camera->CaptureRGBA(&imgRGBA, 1000, true) )
					printf("segnet-camera:  failed to convert from NV12 to RGBA\n");

				//process the segmentation network
				if( !sNet->Process(imgRGBA, camera->GetWidth(), camera->GetHeight()) )
				{
					printf("segnet-console:  failed to process segmentation\n");
					continue;
				}

				if ( !sNet->Mask(mask, (int)camera->GetWidth(), (int)camera->GetHeight()))
				{
					printf("segnet-console: failed to process segmentation mask\n");
					continue;
				}

				CUDA(cudaDeviceSynchronize());

				memset(&message, 0, sizeof(message));
				message.messageType = SharedMemory;
				message.source = TX2Cam;
				write(masterWrite, &message, sizeof(message));
			}
			else if (message.messageType = KillMessage)
			{ 
				killMessageReceived = 1;
				CloseSharedMemory();
				close(masterRead);
				close(masterWrite);
			}
		}
	}

	/*
	 * destroy resources
	 */
	printf("segnet-camera:  shutting down...\n");
	
	SAFE_DELETE(camera);
	//SAFE_DELETE(display);
	SAFE_DELETE(sNet);
	SAFE_DELETE(iNet);

	printf("segnet-camera:  shutdown complete.\n");


	printf("Killing camera node\n");
	return 0;
}

