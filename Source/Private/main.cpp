///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2022, STEREOLABS.
//
// All rights reserved.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////

#include <sl/Camera.hpp>

#include "Roles/LiveLinkCameraRole.h"
#include "Roles/LiveLinkCameraTypes.h"
#include "LiveLinkProvider.h"
#include "RequiredProgramMainCPPInclude.h"
#include "Modules/ModuleManager.h"
#include "LiveLinkRefSkeleton.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"

#include "ZEDCamera.h"
#include "ZEDStructs.h"
#include "Utils.h"

IMPLEMENT_APPLICATION(ZEDLiveLinkPlugin, "ZEDLiveLink");

using namespace sl;
using namespace std;

#define ENABLE_OBJECT_DETECTION 1


static TSharedPtr<ILiveLinkProvider> LiveLinkProvider;

struct StreamedSkeletonData
{
	FName SubjectName;
	double Timestamp;
	TArray<FTransform> Skeleton;

	StreamedSkeletonData() {}
	StreamedSkeletonData(FName inSubjectName) : SubjectName(inSubjectName) {}
};

struct StreamedCameraData
{
	FName SubjectName;
	ZEDCamera* Cam;
	StreamedCameraData() {}
	StreamedCameraData(FName inSubjectName, ZEDCamera* InCam) : SubjectName(inSubjectName), Cam(InCam) {}
};


void LibInit();
void parseArgs(int argc, char** argv, SL_InitParameters& init_parameters, string& pathSVO, string& ip, int& port);
void UpdateCameraStaticData(FName SubjectName);
void UpdateCameraFrameData(FName SubjectName, ZEDCamera& camera);
void UpdateSkeletonStaticData(FName SubjectName);
void UpdateAnimationFrameData(StreamedSkeletonData StreamedSkeleton);
ERROR_CODE PopulateSkeletonsData(ZEDCamera* cam);
ERROR_CODE InitCamera(int argc, char **argv);
FTransform BuildUETransformFromZEDTransform(SL_PoseData& pose);
StreamedSkeletonData BuildSkeletonsTransformFromZEDObjects(SL_ObjectData objectData, double timestamp);

bool IsConnected = false;
int minDistX;
int maxDistX;
int minDistY;
int maxDistY;
int posX;
int posY;
int radius;
bool alreadyTracking = false;
int CurrendID;
FString CamId;
bool occupied = false;
// Streamed Data 
StreamedCameraData StreamedCamera;
TMap<int, StreamedSkeletonData> StreamedSkeletons;

///////////////////////////////////////
///////////// MAIN ////////////////////
///////////////////////////////////////

int main(int argc, char **argv)
{
	CamId = FString(argv[3]);

	std::cout << "Starting ZEDLiveLink tool" << endl;
	cout << "Opening camera..." << endl;
	LibInit();
	LiveLinkProvider = ILiveLinkProvider::CreateLiveLinkProvider(TEXT("ZED_") + CamId);
	//// Create camera
	ERROR_CODE e = InitCamera(argc, argv);
	if (e != ERROR_CODE::SUCCESS) {
		cout << "Error " << e << ", exit program.\n";
		return EXIT_FAILURE;
	}
	cout << "Waiting for connection..." << endl;
	//// Update static camera data.
	if (LiveLinkProvider.IsValid()) {
		UpdateCameraStaticData(StreamedCamera.SubjectName);
	}

	//// Loop
	while (true) {
		//// Display status
		if (LiveLinkProvider->HasConnection()) {
			if (!IsConnected) {
				IsConnected = true;
				cout << "ZEDLiveLink is connected " << endl;
				//CamId = StreamedCamera.SubjectName.ToString();
				cout << "ZED Camera added : " << TCHAR_TO_UTF8(*CamId) << endl;
			}

			// Grab
			///// Update Streamed data
			SL_RuntimeParameters rt_params;
			rt_params.reference_frame = sl::REFERENCE_FRAME::WORLD;
			sl::ERROR_CODE err = StreamedCamera.Cam->Grab(rt_params);
			if (err == sl::ERROR_CODE::SUCCESS) {
				UpdateCameraFrameData(StreamedCamera.SubjectName, *StreamedCamera.Cam);

#if ENABLE_OBJECT_DETECTION
				e = PopulateSkeletonsData(StreamedCamera.Cam);
				if (e != ERROR_CODE::SUCCESS) {
					cout << "Error " << e << ", exit program.\n";
					return EXIT_FAILURE;
				}
				for (auto& it : StreamedSkeletons) {
					UpdateAnimationFrameData(it.Value);
				}
#endif
			}
			else if (err == sl::ERROR_CODE::END_OF_SVOFILE_REACHED) {
				std::cout << "End of SVO reached " << std::endl;
				StreamedCamera.Cam->setSVOPosition(0);
			}
			else {
				std::cout << "Grab Failed " << std::endl;			
			}
		}
		else if (IsConnected == true) {
			cout << "Source ZED removed" << endl;
			IsConnected = false;
		}
	}
	// Disable positional tracking and close the camera
	StreamedCamera.Cam->DisableTracking();
#if ENABLE_OBJECT_DETECTION
	StreamedCamera.Cam->DisableObjectDetection();
#endif
	StreamedCamera.Cam->Close();

	LiveLinkProvider.Reset();
	return EXIT_SUCCESS;
}

//// Initialize tool
void LibInit()
{
	GEngineLoop.PreInit(TEXT("ZEDLiveLink -Messaging"));
	ProcessNewlyLoadedUObjects();
	// Tell the module manager that it may now process newly-loaded UObjects when new C++ modules are loaded
	FModuleManager::Get().StartProcessingNewlyLoadedObjects();
	FModuleManager::Get().LoadModule(TEXT("UdpMessaging"));
}

//// Convert ZED transform to UE transform
FTransform BuildUETransformFromZEDTransform(SL_PoseData& pose)
{
	FTransform UETransform;
	SL_Vector3 zedTranslation = pose.translation;
	FVector UETranslation = FVector(zedTranslation.x, zedTranslation.y, zedTranslation.z);
	UETransform.SetTranslation(UETranslation);
	UETransform.SetRotation(FQuat(pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w));
	UETransform.SetScale3D(FVector(1, 1, 1));
	return UETransform;
}

ERROR_CODE InitCamera(int argc, char **argv)
{
	string pathSVO = "";
	string ip = "";
	int port = 30000;
	ZEDCamera* zed = new ZEDCamera();
	if (!zed->CreateCamera(0, true))
	{
		std::cout << " ERROR :Create cam" << std::endl;
		return sl::ERROR_CODE::FAILURE;
	}
	SL_InitParameters init_params;
	init_params.resolution = sl::RESOLUTION::HD720;
	init_params.camera_fps = 60;
	init_params.coordinate_system = sl::COORDINATE_SYSTEM::LEFT_HANDED_Z_UP;
	init_params.coordinate_unit = sl::UNIT::CENTIMETER;
	init_params.depth_mode = DEPTH_MODE::ULTRA;
	init_params.sdk_verbose = 1;
	parseArgs(argc, argv, init_params, pathSVO, ip, port);
	ERROR_CODE err = zed->Open(init_params, pathSVO.c_str(), ip.c_str(), port);

	if (err != ERROR_CODE::SUCCESS)
	{
		std::cout << "ERROR : Open" << std::endl;
		return err;
	}

	StreamedCamera = StreamedCameraData(FName(FString::FromInt((zed->GetSerialNumber()))), zed);

	SL_PositionalTrackingParameters tracking_param;
	tracking_param.set_floor_as_origin = true;
	tracking_param.enable_pose_smoothing = true;

	err = zed->EnableTracking(tracking_param);
	if (err != ERROR_CODE::SUCCESS)
	{
		std::cout << " ERROR : Enable Tracking" << std::endl;
		return err;
	}

#if ENABLE_OBJECT_DETECTION
	SL_ObjectDetectionParameters obj_det_params;
	obj_det_params.enable_body_fitting = false;
	obj_det_params.enable_tracking = true;
	obj_det_params.model = sl::DETECTION_MODEL::HUMAN_BODY_ACCURATE;
	obj_det_params.body_format = sl::BODY_FORMAT::POSE_34;
	obj_det_params.max_range = 20 * 100;
	err = zed->EnableObjectDetection(obj_det_params);
	if (err != ERROR_CODE::SUCCESS)
	{
		std::cout << "ERROR : Enable OD" << std::endl;
		return err;
	}
#endif
	return err;
}


StreamedSkeletonData BuildSkeletonsTransformFromZEDObjects(SL_ObjectData objectData, double timestamp)
{
	StreamedSkeletonData SkeletonsData = StreamedSkeletonData(FName(FString::FromInt(objectData.id)));
	SkeletonsData.Timestamp = timestamp;
	TMap<FString, FTransform> rigBoneTarget;
	sl::float3 bodyPosition = objectData.keypoint[0];
	sl::float4 bodyRotation = objectData.global_root_orientation;

	for (int i = 0; i < targetBone.Num(); i++)
	{
		rigBoneTarget.Add(targetBone[i], FTransform::Identity);
	}

	rigBoneTarget["PELVIS"].SetLocation(FVector(bodyPosition.x, bodyPosition.y, bodyPosition.z));
	rigBoneTarget["PELVIS"].SetRotation(FQuat(bodyRotation.x, bodyRotation.y, bodyRotation.z, bodyRotation.w));

	for (int i = 1; i < targetBone.Num(); i++)
	{
		sl::float3 localTranslation = objectData.local_position_per_joint[i];
		sl::float4 localRotation = objectData.local_orientation_per_joint[i];

		rigBoneTarget[targetBone[i]].SetLocation(FVector(localTranslation.x, localTranslation.y, localTranslation.z));
		rigBoneTarget[targetBone[i]].SetRotation(FQuat(localRotation.x, localRotation.y, localRotation.z, localRotation.w));
	}

	TArray<FTransform> transforms;
	for (int i = 0; i < targetBone.Num(); i++)
	{
		transforms.Push(rigBoneTarget[targetBone[i]]);
	}
	SkeletonsData.Skeleton = transforms;
	return SkeletonsData;
}

ERROR_CODE PopulateSkeletonsData(ZEDCamera* zed)
{
	ERROR_CODE e = ERROR_CODE::FAILURE;
	SL_ObjectDetectionRuntimeParameters objectTracker_parameters_rt;
	objectTracker_parameters_rt.object_confidence_threshold[(int)sl::OBJECT_CLASS::PERSON] = 70;
	SL_Objects bodies;
	e = zed->RetrieveObjects(objectTracker_parameters_rt, bodies);
	if (e != ERROR_CODE::SUCCESS)
	{
		cout << "ERROR : retrieve objects : " << e << std::endl;
		return e;
	}

	//std::cout << "No of Objects: " << bodies.nb_object << " Current tracked ID: " << CurrendID << "Occupied: " << occupied << endl;
	
	if (bodies.is_new == 1)
	{
		TArray<int> remainingKeyList;
		StreamedSkeletons.GetKeys(remainingKeyList);
		

		for (int i = 0; i < bodies.nb_object; i++)
		{
			SL_ObjectData objectData = bodies.object_list[i];
		
			if (objectData.tracking_state == sl::OBJECT_TRACKING_STATE::OK && objectData.position.x >= minDistX && objectData.position.x <= maxDistX && objectData.position.y >= minDistY && objectData.position.y <= maxDistY) // Detect if object has entered the active zone
			{
				std::cout << "Object: " << objectData.id << " X: " + to_string(objectData.position.x) + " Y: " + to_string(objectData.position.y) + " L Hand Z: " + to_string(objectData.position.y) << " IN" << " Current ID: " << CurrendID<< endl;
			
				if (!StreamedSkeletons.Contains(objectData.id) && !occupied)  // If it's a new ID and zone is not occupied, add new object and set occupancy
				
				{
					occupied = true;
					CurrendID = objectData.id;
					UpdateSkeletonStaticData(FName(CamId + "_" + FString::FromInt(objectData.id)));
					StreamedSkeletonData data = BuildSkeletonsTransformFromZEDObjects(objectData, bodies.image_ts);
					StreamedSkeletons.Add(objectData.id, data);
				}

				else if(occupied && objectData.id == CurrendID) //if new object added and zone occpied with current id, stream data
				{
					StreamedSkeletons[objectData.id] = BuildSkeletonsTransformFromZEDObjects(objectData, bodies.image_ts);
					remainingKeyList.Remove(objectData.id);

				}
			}

			else if(objectData.id == CurrendID) // reset occupancy if current object has exited active zone
			{
				std::cout << "Object: " << objectData.id << " X: " + to_string(objectData.position.x) + " Y: " + to_string(objectData.position.y) << " OUT" << endl;
				occupied = false;
			}
		  
		}
		for (int index = 0; index < remainingKeyList.Num(); index++)
		{
			LiveLinkProvider->RemoveSubject(FName(CamId + "_" + FString::FromInt(remainingKeyList[index])));
			StreamedSkeletons.Remove(remainingKeyList[index]);
			
		}
	}
	return e;
}

//// Update Camera static data 
void UpdateCameraStaticData(FName SubjectName)
{
	FLiveLinkStaticDataStruct StaticData(FLiveLinkCameraStaticData::StaticStruct());
	FLiveLinkCameraStaticData& CameraData = *StaticData.Cast<FLiveLinkCameraStaticData>();
	CameraData.bIsAspectRatioSupported = true;
	CameraData.bIsFieldOfViewSupported = true;
	CameraData.bIsFocalLengthSupported = false;
	CameraData.bIsFocusDistanceSupported = false;
	CameraData.bIsProjectionModeSupported = true;
	LiveLinkProvider->UpdateSubjectStaticData(SubjectName, ULiveLinkCameraRole::StaticClass(), MoveTemp(StaticData));
}

//// Update Camera Frame data
void UpdateCameraFrameData(FName SubjectName, ZEDCamera& zed)
{
	FLiveLinkFrameDataStruct FrameData(FLiveLinkCameraFrameData::StaticStruct());
	FLiveLinkCameraFrameData& CameraData = *FrameData.Cast<FLiveLinkCameraFrameData>();
	SL_PoseData pose;
	zed.GetPosition(pose, sl::REFERENCE_FRAME::WORLD);
	FTransform Pose = BuildUETransformFromZEDTransform(pose);
	CameraData.AspectRatio = 16. / 9;
	//CameraData.FieldOfView = zed.getCameraInformation().camera_configuration.calibration_parameters.left_cam.h_fov;
	CameraData.ProjectionMode = ELiveLinkCameraProjectionMode::Perspective;
	CameraData.Transform = Pose;
	double StreamTime = FPlatformTime::Seconds();
	CameraData.WorldTime = StreamTime;
	LiveLinkProvider->UpdateSubjectFrameData(SubjectName, MoveTemp(FrameData));
}

//// Update Skeleton static data 
void UpdateSkeletonStaticData(FName SubjectName)
{
	FLiveLinkStaticDataStruct StaticData(FLiveLinkSkeletonStaticData::StaticStruct());
	FLiveLinkSkeletonStaticData& AnimationData = *StaticData.Cast<FLiveLinkSkeletonStaticData>();
	for (int i = 0; i < targetBone.Num(); i++)
	{
		AnimationData.BoneNames.Add(FName(targetBone[i]));
		AnimationData.BoneParents.Add(parentsIdx[i]);
	}

	LiveLinkProvider->UpdateSubjectStaticData(SubjectName, ULiveLinkAnimationRole::StaticClass(), MoveTemp(StaticData));
}

//// Update Skeleton frame data 
void UpdateAnimationFrameData(StreamedSkeletonData StreamedSkeleton)
{
	FLiveLinkFrameDataStruct FrameData(FLiveLinkAnimationFrameData::StaticStruct());
	FLiveLinkAnimationFrameData& AnimationData = *FrameData.Cast<FLiveLinkAnimationFrameData>();

	double timestamp = (StreamedSkeleton.Timestamp / 1000000000.0f);// ns to seconds
	double StreamTime = FPlatformTime::Seconds();
	AnimationData.WorldTime = StreamTime;
	AnimationData.Transforms = StreamedSkeleton.Skeleton;
	FString  newSubjectName = CamId + "_" + StreamedSkeleton.SubjectName.ToString();
	LiveLinkProvider->UpdateSubjectFrameData(FName(*newSubjectName), MoveTemp(FrameData));

}


void parseArgs(int argc, char **argv, SL_InitParameters& param, string& pathSVO, string& ip, int& port)
{

	sscanf(argv[2], "%u,%u,%u", &posX, &posY, &radius);
	
	minDistX = posX - radius;
	maxDistX = posX + radius;
	minDistY = posY - radius;
	maxDistY = posY + radius;

	//CamId = FString(argv[3]);

	cout << "Active Zone Version 2.0"<< endl;
    cout << "Using active zone at : " << to_string(posX) + "," + to_string(posY) + "," + to_string(radius)<< endl;
	
	if (argc > 1 && string(argv[1]).find(".svo") != string::npos) {
		// SVO input mode
		param.input_type = sl::INPUT_TYPE::SVO;
		pathSVO = string(argv[1]);
		cout << "Using SVO File input: " << argv[1] << endl;
	}

	else if (argc > 1 && string(argv[1]).find(".svo") == string::npos) {
		string arg = string(argv[1]);
		
		unsigned int a, b, c, d, p;

		if (sscanf(arg.c_str(), "%u.%u.%u.%u:%d", &a, &b, &c, &d, &p) == 5) {
			// Stream input mode - IP + port
			string ip_adress = to_string(a) + "." + to_string(b) + "." + to_string(c) + "." + to_string(d);
			param.input_type = sl::INPUT_TYPE::STREAM;
			ip = string(ip_adress);
			port = p;
			cout << "Using Stream input, IP : " << ip_adress << ", port : " << p << endl;
		}
		else if (sscanf(arg.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
			// Stream input mode - IP only
			param.input_type = sl::INPUT_TYPE::STREAM;
			ip = argv[1];
			cout << "Using Stream input, IP : " << argv[1] << endl;
		}
		else if (arg.find("HD2K") != string::npos) {
			param.resolution = RESOLUTION::HD2K;
			cout << "Using Camera in resolution HD2K" << endl;
		}
		else if (arg.find("HD1080") != string::npos) {
			param.resolution = RESOLUTION::HD1080;
			cout << "Using Camera in resolution HD1080" << endl;
		}
		else if (arg.find("HD720") != string::npos) {
			param.resolution = RESOLUTION::HD720;
			cout << "Using Camera in resolution HD720" << endl;
		}
		else if (arg.find("VGA") != string::npos) {
			param.resolution = RESOLUTION::VGA;
			cout << "Using Camera in resolution VGA" << endl;
		}
		
		

	}
}