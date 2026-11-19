#define TEST_AVAILABILITY 1
#define EMCMOT_AVAILABILITY_AVAILABLE 3
typedef struct { int controlled, state; double held_position; } availability_status_t;
static struct { EmcPose carte_pos_cmd; int overrideLimitMask; struct { availability_status_t availability; } joint_status[3]; } status;
#define emcmotStatus (&status)
