#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <mips/trapframe.h>
#include "array.h"
#include "synch.h"
#include "opt-A2.h"

void sys__exit(int exitcode) {
  DEBUG(DB_SYSCALL,"sys__exit: (%d)\n", exitcode);

  struct addrspace *as;
  struct proc *p = curproc;

#if OPT_A2 
  p->alive = false;

  lock_acquire(p->lk);
  cv_signal(p->cv, p->lk);
  bool alive = false;
  int n = array_num(p->children);
  for (int i = 0; i < n; i++) {
    alive = ((struct resid *) array_get(p->children, i))->alive || alive;
  }
  lock_release(p->lk);

  lock_acquire(p->lk);
  if (!alive) {

    while (n > 0) {
      // TODO(): change this to delete the info object instead...
      proc_destroy(array_get(p->children, n - 1));
      n--;
      array_setsize(p->children, n);
    }

    array_destroy(p->children);

    // DEBUG(DB_SYSCALL, "successfully destroyed children");
  }
  lock_release(p->lk);
  p->exitStatus = exitcode;
#else
  exitcode = 0;
#endif


  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
#if OPT_A2 
  if (!(p->parent && p->parent->alive)) proc_destroy(p);
#else
  proc_destroy(p);
#endif
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  // DEBUG(DB_SYSCALL,"sys_getpid: (%d)\n", *retval);
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  #if OPT_A2
    *retval = curproc->pid;
  #else
    *retval = 1;
  #endif
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  // DEBUG(DB_SYSCALL,"sys_waitpid: (%d)\n", *retval);
  int exitstatus;
  int result;

  if (options != 0) {
    return(EINVAL);
  }

  #if OPT_A2

    struct proc *p = curproc;

    lock_acquire(p->lk);
    int n = array_num(p->children);

    for (int i = 0; i < n; i++) {
      struct proc *child = array_get(p->children, i);

      if (child->pid == pid) {
        while (child->alive) cv_wait(child->cv, child->lk);
        exitstatus = _MKWAIT_EXIT(child->exitStatus);
        proc_destroy(child);
      }
    }

    lock_release(p->lk);

  #else
    exitstatus = 0;
  #endif

  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}


#if OPT_A2
int sys_fork(struct trapframe *tf, pid_t *retval) {

  struct proc *child = proc_create_runprogram(curproc->p_name);
  KASSERT(child && child->pid);

  struct resid *residual = kmalloc(sizeof(struct resid));
  residual->ref = child;
  residual->pid = pid;
  residual->exitStatus = 0;
  residual->alive = true;
  
  struct addrspace *as = as_create(); 
  KASSERT(as);

  // lock_acquire(curproc->lk);
  // as_copy(curproc->p_addrspace, &as);
  if (as_copy(curproc->p_addrspace, &as) != 0) {
    return ENOMEM;
  }
  child->p_addrspace = as; 
  // lock_release(curproc->lk);
 
  // lock_acquire(curproc->lk);
  child->parent = curproc;

  array_add(curproc->children, child->resid, NULL);
  // lock_release(curproc->lk);

  struct trapframe *temp= kmalloc(sizeof(struct trapframe));
  KASSERT(temp);
  *temp = *tf;

  thread_fork(child->p_name, child, &enter_forked_process, temp, 15); 

  *retval = child->pid;

  return 0;
}
#endif
