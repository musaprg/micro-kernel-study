#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>
#include <signal.h>

#ifdef __linux__
#undef setjmp
#endif

#define TCB_MAXNUM 16

struct task_define {
  char *name;
  int (*mainfunc)(int argc, char *argv[]);
  int stack_size;
};

typedef enum {
    TCB_STATUS_NONE = 0,
    TCB_STATUS_ACTIVE,
    TCB_STATUS_SLEEP,
    TCB_STATUS_EXITED,
} tcb_status_t;

typedef enum {
    TCB_SYSCALL_EXIT,
    TCB_SYSCALL_SLEEP,
    TCB_SYSCALL_WAKEUP,
    TCB_SYSCALL_SEND,
    TCB_SYSCALL_RECV,
} tcb_syscall_t;

struct task_control_block {
  struct task_define *task;
  tcb_status_t status;
  int exit_code;
  struct {
    tcb_syscall_t code;
    long args[8];
    long ret;
  } syscall;
  jmp_buf context;
} tcb[TCB_MAXNUM];

struct task_control_block *current;
struct task_control_block *max;

struct msgbuf {
  struct msgbuf *next;
};

struct msgbox {
  struct task_control_block *receiver;
  struct msgbuf *head;
  struct msgbuf *tail;
};

typedef enum {
  MSGBOX_ID_SAMPLE = 0,
  MSGBOX_ID_NUM
} msgbox_id_t;

struct msgbox msgboxes[MSGBOX_ID_NUM];

void schedule()
{
  int i;
  for (i = 0; i < TCB_MAXNUM; i++) {
    current++;
    if (current == max)
      current = tcb;
    if (current->status == TCB_STATUS_ACTIVE)
      return;
  }
  exit(0);
}

void dispatch()
{
  longjmp(current->context, 1);
}

void context_switch()
{
  if (setjmp(current->context) != 0)
    return;
  schedule();
  dispatch();
}

int syscall_exit(int code)
{
  current->syscall.code = TCB_SYSCALL_EXIT;
  current->syscall.args[0] = code;
  kill(getpid(), SIGSYS);
  return current->syscall.ret;
}

int syscall_sleep()
{
  current->syscall.code = TCB_SYSCALL_SLEEP;
  kill(getpid(), SIGSYS);
  return current->syscall.ret;
}

int syscall_wakeup(struct task_control_block *tcb)
{
  current->syscall.code = TCB_SYSCALL_WAKEUP;
  current->syscall.args[0] = (long)tcb;
  kill(getpid(), SIGSYS);
  return current->syscall.ret;
}

int syscall_send(msgbox_id_t id, struct msgbuf *mp)
{
  current->syscall.code = TCB_SYSCALL_SEND;
  current->syscall.args[0] = id;
  current->syscall.args[1] = (long)mp;
  kill(getpid(), SIGSYS);
  return current->syscall.ret;
}

struct msgbuf *syscall_recv(msgbox_id_t id)
{
  current->syscall.code = TCB_SYSCALL_RECV;
  current->syscall.args[0] = id;
  kill(getpid(), SIGSYS);
  return (struct msgbuf *)current->syscall.ret;
}

int task1_main(int argc, char *argv[])
{
  int i;
  struct msgbuf msg;

  for (i = 0; i < 20; i++) {
    fprintf(stderr, "task 1 %s %p\n", argv[0], current);
    sleep(1);
    if (i == 10) {
      fprintf(stderr, "message send %p\n", &msg);
      syscall_send(MSGBOX_ID_SAMPLE, &msg);
    }
  }

  return 0;
}

int task2_main(int argc, char *argv[])
{
  int i;

  for (i = 0; i < 3; i++) {
    fprintf(stderr, "task 2 %s %p\n", argv[0], current);
    sleep(1);
  }

  return 0;
}

int task3_main(int argc, char *argv[])
{
  int i;
  struct msgbuf *mp;

  for (i = 0; i < 5; i++) {
    fprintf(stderr, "task 3 %s %p\n", argv[0], current);
    if (i == 1) {
      mp = syscall_recv(MSGBOX_ID_SAMPLE);
      fprintf(stderr, "message recv %p\n", mp);
    }
    sleep(1);
  }

  return 0;
}

struct task_define tasks[] = {
  { "task1", task1_main, 4096 },
  { "task2", task2_main, 4096 },
  { "task3", task3_main, 4096 },
  { NULL, NULL, 0 }
};

void sigint_handler(int value)
{
  context_switch();
}

void sigalrm_handler(int value)
{
  alarm(2);
  context_switch();
}

void sigsegv_handler(int value)
{
  current->status = TCB_STATUS_SLEEP;
  context_switch();
}

long syscall_exit_proc(long args[])
{
  current->status = TCB_STATUS_EXITED;
  current->exit_code = args[0];
  return 0;
}

long syscall_sleep_proc(long args[])
{
  current->status = TCB_STATUS_SLEEP;
  return 0;
}

long syscall_wakeup_proc(long args[])
{
  struct task_control_block *tcb;
  tcb = (struct task_control_block *)args[0];
  tcb->status = TCB_STATUS_ACTIVE;
  return 0;
}

struct msgbuf *recvmsg(struct msgbox *mboxp)
{
  struct msgbuf *mp;

  mp = mboxp->head;
  mboxp->head = mp->next;
  if (mboxp->head == NULL)
    mboxp->tail = NULL;
  mp->next = NULL;

  return mp;
}

long syscall_send_proc(long args[])
{
  struct msgbox *mboxp = &msgboxes[args[0]];
  struct msgbuf *mp = (struct msgbuf *)args[1];

  mp->next = NULL;

  if (mboxp->tail) {
    mboxp->tail->next = mp;
  } else {
    mboxp->head = mp;
  }
  mboxp->tail = mp;

  if (mboxp->receiver) {
    mp = recvmsg(mboxp);
    mboxp->receiver->syscall.ret = (long)mp;
    mboxp->receiver->status = TCB_STATUS_ACTIVE;
    mboxp->receiver = NULL;
  }

  return 0;
}

long syscall_recv_proc(long args[])
{
  struct msgbox *mboxp = &msgboxes[args[0]];
  struct msgbuf *mp;

  if (mboxp->receiver)
    return 0;

  mboxp->receiver = current;

  if (mboxp->head == NULL) {
    current->status = TCB_STATUS_SLEEP;
    return 0;
  }

  mp = recvmsg(mboxp);
  mboxp->receiver = NULL;

  return (long)mp;
}

typedef long (*syscall_proc_t)(long args[]);

syscall_proc_t syscall_table[] = {
  syscall_exit_proc,
  syscall_sleep_proc,
  syscall_wakeup_proc,
  syscall_send_proc,
  syscall_recv_proc,
};

void sigsys_handler(int value)
{
  syscall_proc_t proc;
  proc = syscall_table[current->syscall.code];
  current->syscall.ret = proc(current->syscall.args);
  context_switch();
}

void start()
{
  max = current;
  current = tcb;

  signal(SIGINT, sigint_handler);
  signal(SIGALRM, sigalrm_handler);
  signal(SIGBUS, sigsegv_handler);
  signal(SIGSEGV, sigsegv_handler);
  signal(SIGSYS, sigsys_handler);

  alarm(2);

  dispatch();
}

void task_create(struct task_define *task);

void expand_stack(int size, struct task_define *task, char *dummy)
{
  char stack[1024];
  if (size < 0) {
    current++;
    task_create(++task);
  }
  expand_stack(size - sizeof(stack), task, stack);
}

void task_create(struct task_define *task)
{
  char *args[2];

  if (task->name == NULL)
    start();

  current->task = task;
  current->status = TCB_STATUS_ACTIVE;

  if (setjmp(current->context) == 0)
    expand_stack(task->stack_size, task, NULL);

  args[0] = current->task->name;
  args[1] = NULL;

  syscall_exit(current->task->mainfunc(1, args));
}

int main()
{
  memset(tcb, 0, sizeof(tcb));
  memset(msgboxes, 0, sizeof(msgboxes));
  current = tcb;
  task_create(tasks);
  return 0;
}
