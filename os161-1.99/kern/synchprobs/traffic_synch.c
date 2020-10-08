#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>

/* 
 * This simple default synchronization mechanism allows only vehicle at a time
 * into the intersection.   The intersectionSem is used as a a lock.
 * We use a semaphore rather than a lock so that this code will work even
 * before locks are implemented.
 */

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */

/*
 * replace this with declarations of any synchronization and other variables you need here
 */
// enum Direction{NORTH, EAST, SOUTH, WEST}; 

volatile int numAllowed = 0; 
volatile Direction direction = north;

volatile int waitTimes[4] = {0, 0, 0, 0};

int states[4] = {0, 0, 0, 0};
struct cv *cvs[4];
static struct lock *intersectionLock;

bool isFirstCar = true;

/* 
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */
void
intersection_sync_init(void)
{
  /* replace this default implementation with your own implementation */

  intersectionLock = lock_create("intersectionLock");
  if (!intersectionLock) {
    panic("could not create intersection intersectionLock");
  }

  cvs[0] = cv_create("north");
  cvs[1] = cv_create("east");
  cvs[2] = cv_create("south");
  cvs[3] = cv_create("west");

  for (int i = 0; i < 4; i++) {
    if (!cvs[i]) panic("could not create cv");
  }
}

/* 
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
void
intersection_sync_cleanup(void)
{
  /* replace this default implementation with your own implementation */
  KASSERT(intersectionLock != NULL);

  lock_destroy(intersectionLock);
  for (int i = 0; i < 4; i++) cv_destroy(cvs[i]);
}

// helper for intersection_before_entry
bool check_other_directions(int index) {
  for (int i = 0; i < 4; i++) {
    if (i != index && states[i]) return true;
  }
  return false;
}

/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread 
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */

void
intersection_before_entry(Direction origin, Direction destination) 
{
  /* replace this default implementation with your own implementation */
  (void)destination; /* avoid compiler complaint about unused parameter */
  KASSERT(intersectionLock != NULL);

  lock_acquire(intersectionLock);

  if (isFirstCar) {
    isFirstCar = false;
    direction = origin;
  }

  while (check_other_directions(origin)) {
    waitTimes[origin] += 1;
    cv_wait(cvs[origin], intersectionLock);
  }
  waitTimes[origin] = 0;
  states[origin]++;
  numAllowed++;

  lock_release(intersectionLock);
}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

void
intersection_after_exit(Direction origin, Direction destination) 
{
  KASSERT(intersectionLock != NULL && states[origin] > 0);
  (void)destination;
  lock_acquire(intersectionLock);

  if (states[origin] == 0) {
    int rightI = (origin + 1) % 4;
    int oppositeI = (origin + 2) % 4;
    int leftI = (origin + 3) % 4;
    int right = waitTimes[rightI];
    int opposite = waitTimes[oppositeI];
    int left = waitTimes[leftI];
 
    if (right >= opposite && right >= left) {
      cv_broadcast(cvs[rightI], intersectionLock);
    } else if (left >= right && left >= opposite) {
      cv_broadcast(cvs[leftI], intersectionLock);
    } else {
      cv_broadcast(cvs[oppositeI], intersectionLock);
    }
  }

  states[origin]--;
  numAllowed--;
  isFirstCar = check_other_directions(-1) && numAllowed == 0;

  for (int i = 0; i < 4; i++) cv_broadcast(cvs[i], intersectionLock);
  lock_release(intersectionLock);
}
