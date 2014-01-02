/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <string.h>
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

extern void write_verify(unsigned long address);

long last_pid=0;

void verify_area(void * addr,int size)
{
    unsigned long start;

    start = (unsigned long) addr;
    size += start & 0xfff;
    start &= 0xfffff000;
    start += get_base(current->ldt[2]);
    while (size>0) {
        size -= 4096;
        write_verify(start);
        start += 4096;
    }
}

// child process's code segment, data segment and copy the
// child process's first page table (from parent's page table)
int copy_mem(int nr,struct task_struct * p)
{
    unsigned long old_data_base,new_data_base,data_limit;
    unsigned long old_code_base,new_code_base,code_limit;

    // get the limit of code and data segment
    code_limit=get_limit(0x0f);
    data_limit=get_limit(0x17);

    // parent process's code segment and data segment's addr
    old_code_base = get_base(current->ldt[1]); // ldt[1] code
    old_data_base = get_base(current->ldt[2]); // ldt[2] data
    if (old_data_base != old_code_base)
        panic("We don't support separate I&D");
    if (data_limit < code_limit)
        panic("Bad data_limit");
    // 0x4000000 is 64MB
    new_data_base = new_code_base = nr * 0x4000000;
    p->start_code = new_code_base;
    set_base(p->ldt[1],new_code_base); // set base of code segment
    set_base(p->ldt[2],new_data_base); // set base of data segment
    if (copy_page_tables(old_data_base,new_data_base,data_limit)) {
        printk("free_page_tables: from copy_mem\n");
        free_page_tables(new_data_base,data_limit);
        return -ENOMEM;
    }
    return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 *
 */
int copy_process(int nr,long ebp,long edi,long esi,long gs,long none,
        long ebx,long ecx,long edx,
        long fs,long es,long ds,
        long eip,long cs,long eflags,long esp,long ss) {
    //the parameters are pushed by int 0x80 system_call, and sys_fork
    struct task_struct *p;
    int i;
    struct file *f;

    // get most significant page, use this page as task_union
    p = (struct task_struct *) get_free_page();
    if (!p)
        return -EAGAIN;
    task[nr] = p; //task[nr]'s task struct

    // NOTE!: the following statement now work with gcc 4.3.2 now, and you
    // must compile _THIS_ memcpy without no -O of gcc.#ifndef GCC4_3
    // current point to task_struct of current process. Now child and
    // parent processes have exactly the same task struct
    // current: pointer to current process, p pointer to the new process
    *p = *current;    /* NOTE! this doesn't copy the supervisor stack */
                      /* This only copied the task_struct, the kernel
                       * stack haven't been copied */

    p->state = TASK_UNINTERRUPTIBLE; //only wake up when it is set to ready state
    p->pid = last_pid; // customized for child process
    p->father = current->pid;
    p->counter = p->priority;
    p->signal = 0;
    p->alarm = 0;
    p->leader = 0;        /* process leadership doesn't inherit */
    p->utime = p->stime = 0;
    p->cutime = p->cstime = 0;
    p->start_time = jiffies;

    // child process's ts
    p->tss.back_link = 0;
    p->tss.esp0 = PAGE_SIZE + (long) p;
    p->tss.ss0 = 0x10;
    p->tss.eip = eip;
    p->tss.eflags = eflags;
    p->tss.eax = 0; // if(__res >= 0), 2nd time execute fork()
    p->tss.ecx = ecx;
    p->tss.edx = edx;
    p->tss.ebx = ebx;
    p->tss.esp = esp;
    p->tss.ebp = ebp;
    p->tss.esi = esi;
    p->tss.edi = edi;
    p->tss.es = es & 0xffff;
    p->tss.cs = cs & 0xffff;
    p->tss.ss = ss & 0xffff;
    p->tss.ds = ds & 0xffff;
    p->tss.fs = fs & 0xffff;
    p->tss.gs = gs & 0xffff;
    p->tss.ldt = _LDT(nr);
    p->tss.trace_bitmap = 0x80000000;

    if (last_task_used_math == current)
        __asm__("clts ; fnsave %0"::"m" (p->tss.i387));

    // setup the code segment, data segment and create the
    // first page table of child process
    if (copy_mem(nr,p)) {
        task[nr] = NULL;
        free_page((long) p);
        return -EAGAIN;
    }

    // increase the count of file reference,
    // since the child process also referencing the
    // file that referenced by its parent.
    for (i=0; i<NR_OPEN;i++)
        if ((f=p->filp[i]))
            f->f_count++;
    if (current->pwd)
        current->pwd->i_count++;
    if (current->root)
        current->root->i_count++;
    if (current->executable)
        current->executable->i_count++;
    // setup GDT's child process
    set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
    set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));
    p->state = TASK_RUNNING;    /* do this last, just in case */
    return last_pid;
}

int find_empty_process(void) //find a vacant position for new process
{
    int i;

    repeat:
        if ((++last_pid)<0) last_pid=1;                           // if overflowed, assign 1
        for(i=0 ; i<NR_TASKS ; i++)                               // NR_TASKS is 64
            if (task[i] && task[i]->pid == last_pid) goto repeat;
    for(i=1 ; i<NR_TASKS ; i++)                                   // find the first empty i
        if (!task[i])
            return i;
    return -EAGAIN;                                               // EAGAIN is 11
}
