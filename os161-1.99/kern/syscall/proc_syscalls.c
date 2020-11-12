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
#include <vfs.h>
#include <limits.h>
#include <kern/fcntl.h>
#include "array.h"
#include "synch.h"
#include "opt-A2.h"

void sys__exit(int exitcode) {
  // DEBUG(DB_SYSCALL,"sys__exit: (%d)\n", exitcode);

  struct addrspace *as;
  struct proc *p = curproc;

#if OPT_A2
  // we're checking if any other children are still alive before we destroy everything
  // note: the suggestion to move pid, alive, and exitStatus to another object was too hard to figure out
  p->alive = false;

  lock_acquire(p->lk);
  cv_signal(p->cv, p->lk);
  bool alive = false;
  int n = array_num(p->children);

  for (int i = 0; i < n; i++) {
    alive = ((struct proc *) array_get(p->children, i))->alive || alive;
  }
  lock_release(p->lk);

  lock_acquire(p->lk);
  if (!alive) {

    while (n > 0) {
      proc_destroy(array_get(p->children, n - 1));
      n--;
      array_setsize(p->children, n);
    }

    array_destroy(p->children);
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
  // postmortem:
  // apparently the reason why my onefork worked and my widefork didn't
  // was the result of me not allocating enough ram...
  // 50 hours and lots of consultations through discord and piazza were had
  // thanks guys I appreciate it though
  // UPDATE: i got 50% on my previous thing because of some issue with my lock_do_i_hold... really tragic

  struct trapframe *temp= kmalloc(sizeof(struct trapframe));
  KASSERT(temp);
  *temp = *tf;

  struct proc *child = proc_create_runprogram(curproc->p_name);
  KASSERT(child && child->pid);
  
  struct addrspace *as = as_create(); 
  KASSERT(as);

  if (as_copy(curproc->p_addrspace, &as) != 0) {
    return ENOMEM;
  }
  child->p_addrspace = as; 
  child->parent = curproc;

  array_add(curproc->children, child, NULL);

  thread_fork(child->p_name, child, &enter_forked_process, temp, 15); 
  *retval = child->pid;

  return 0;
}

int sys_execv(userptr_t progname, char **args) {
  struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

  int argc = 0;
  while (args[argc]) argc++;

  size_t allocSize = sizeof(char*) * (argc + 1);
  size_t actual;
  size_t len;

  char** argv = kmalloc(allocSize);

  argv[argc] = NULL;
  for (int i = 0; i < argc; i++) {
    len = strlen(args[i]) + 1;
    argv[i] = kmalloc(len * sizeof(char));
    result = copyinstr((userptr_t) args[i], argv[i], len, &actual);
    if (result) {
      return result;
    }
  }

	/* Open the file. */
	result = vfs_open((char *) progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* We should be a new process. */
	// KASSERT(curproc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as ==NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

	vaddr_t *stackptrs = kmalloc(allocSize);

	for (int i = 0; i <= argc; i++) {
		if (i == argc) {
			stackptrs[i] = (vaddr_t)NULL;
			stackptr = ROUNDUP(stackptr, 8) - allocSize - 2 * sizeof(char *);
			break;
		}
		size_t len = strlen(argv[i]) + 1;
		userptr_t newStackptr = (userptr_t) stackptr - len;
		int err = copyout(argv[i], newStackptr, len);
		KASSERT(!err);
		stackptrs[i] = (int) newStackptr;
		stackptr = stackptrs[i];
	}

	for (int i = 0; i <= argc; i++) {
		int err = copyout(&stackptrs[i], (userptr_t) stackptr + 4 * i, 4);
		KASSERT(!err);
	}

  for (int i = 0; i < argc; i++) kfree(argv[i]);
  kfree(argv);
  kfree(stackptrs);

	/* Warp to user mode. */
	enter_new_process(argc /*argc*/, (userptr_t) stackptr /*userspace addr of argv*/,
			  stackptr, entrypoint);
	
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;

}

#endif
