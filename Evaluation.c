/*
    Pierre Boisselier - IN501A11
    =============================

    + Add segfault notification
    + More error handling around wait and kill
    + Quit with jobs should warn or block

    * cat < f1 | cat -n | tee f2 | cat -n | (sleep 4 && cat -n > f4)
      Kinda works but hangs (pipe not closed), but ctrl-c then fg works to
        resume and kill Same in non-interactive

    * (sleep 2 && cat < f1) &
      Does not print unless resumed (suspended by default TTOU)
      Works wonderful in non-interactive
      fg + ctrl-z fucks up everything

    - Check for async signal safe functions
    https://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html#tag_15_04_03_03
      Warn: shared varialbe is not async safe for signals

    SA_NOCLDWAIT, SA_RESTART?
*/

#include "Evaluation.h"

#include "Shell.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/*=====================*/
/* BEGIN DECLARATIONS  */
/*=====================*/

/* Pipe2 should be portable enough to not care, especially with
 * _XOPEN_SOURCE=700 */
/* Linux >= 2.9
 * OpenBSD >= 5.7
 * FreeBSD >= 10.0
 * MacOS ... nothing */
#ifndef __APPLE_
extern int pipe2 (int pipefd[2], int flags);
#else
/* Not converting flag as the only use of pipe2 is for O_CLOEXEC */
static int
pipe2 (int pipefd[2], int flags)
{
    if (pipe (pipefd) < 0)
        return -1;
    if (fcntl (pipefd[0], F_SETFL, FD_CLOEXEC) < 0)
        return -1;
    if (fcntl (pipefd[1], F_SETFL, FD_CLOEXEC) < 0)
        return -1;
    return 0;
}
#endif

/***********/
/* Helpers */
/***********/

/* Put code here directly, it's a kind of "inline" */

/* Internal status used */
#define INTERNSTATUS -128
/* Convert exit status to correct one */
#define STATUS(_status_)                        \
    do                                          \
    {                                           \
        if (_status_ < 0)                       \
            _status_ = _status_ - INTERNSTATUS; \
        else                                    \
            _status_ = _status_;                \
    } while (0)

/* Command hash */
#define CD 0x15d9 /* cd */
#define CBG 0x1665 /* bg */
#define CFG 0x1681 /* fg */
#define HASH 0x47ee6 /* hash */
#define HELP 0x4c151 /* help */
#define ECHO 0x4b21d /* echo */
#define EXIT 0x4e65e /* exit */
#define JOBS 0x4d206 /* jobs */
#define ECHO_STATUS 0xd0b /* $? */

/* Help strings */
static const char* internal_help[] = {
    "cd [dir]",
    "echo [$? | arg ...]",
    "exit",
    "hash [text]\t /!\\ Only adds each ASCII character!",
    "fg [name]",
    "bg [name]",
    "help",
    NULL};

/* Display help strings */
static void
dipslay_help (void)
{
    const char* str;
    const char** array = internal_help;
    fprintf (stdout,
             "MiniShell - ProgSys 2020-21\nPierre Boisselier "
             "<pierre.boisselier@etu.u-bordeaux.fr>\n\nThose shell commands "
             "are defined "
             "internally.\n\n");
    while ((str = *array++))
        fprintf (stdout, "\t%s\n", str);
    fprintf (stdout, "\n");
    fprintf (stdout,
             "Keyboard shortcuts:\n\t- Ctrl-Z: Suspend current job in "
             "foreground\n\t- Ctrl-C: Interrupt current foreground job\n\n");
}

/*
 * Provides a basic hash function
 * Just adds every ASCII code
 * 100% cryptographically secure :)
 */
static const int
hash_cmd (const char* str)
{
    /* There shouldn't be any collisions, but still not really the best hash
     * function */
    int hash = 0;
    int i    = 7;
    int c;
    while ((c = *str++))
    {
        hash = (hash + c * i) % INT_MAX;
        i *= 7;
    }
    return hash;
}

/***************/
/* Job control */
/***************/

/* Design Choice: Using linked list for job control would be better.
 * However, this Shell being a "mini"-one there is no need for "unlimited" jobs.
 * MAXJOBS is enough. */

/* There is WNOHANG but no WHANG and that's sad */
#define WHANG 0

/* Start in foreground */
#define JFG 0
/* Start in background */
#define JBG 1
/* Maximum number of concurrent jobs */
#define MAXJOBS 32
/* Size of command name buffer for each job */
#define CMDBUFSZ 16

/* All states a job can be (and can dream of) */
enum state_t
{
    JDONE,
    JRUNNING,
    JSTOPPED
};

typedef struct job
{
    int jid;            /* Job id */
    pid_t pid;          /* pid, if 0 then free slot */
    pid_t pgid;         /* Group pid */
    int background;     /* Is in background? */
    int state;          /* See state_t */
    int status;         /* Return status, -1 by default */
    int termsig;        /* If > 0, the signal received is here */
    char cmd[CMDBUFSZ]; /* Only 16 first characters of cmd */
} job_t;

/* Shell PID */
static int shpid;
/* Flag set when init_shell was ran*/
static int init_flag = 0;
/* Interactive mode flag */
static int interactive = 1;

/* See Design Choice above */
static job_t job_list[MAXJOBS];
/* Last background job started */
static job_t* last_job = job_list;
/* Current foreground job */
static job_t* fg_job = NULL;

/* Initialize shell */
static int init_shell (void);
/* Register a job */
static job_t* register_job (pid_t pid, pid_t pgid, int background, char* cmd);
/* Start a job */
static void launch_job (job_t* job, int notify);
/* Unregister a job */
static void unregister_job (job_t* job);
/* Remove jobs done */
static void remove_old_jobs (int notify);
/* Find a job from a pid */
static job_t* find_job (pid_t pid);
/* Suspend a job with ctrl-z */
static void suspend_job (job_t* job);
/* Set the exit status of a job */
static inline void set_status_job (job_t* job, int wstatus);
/* Send to foregound (and continue) */
static void send_to_foreground (job_t* job);
/* Send to background (and continue) */
static void send_to_background (job_t* job);
/* Display a job */
static void display_job (const job_t* job);

/*******************/
/* Signal handling */
/*******************/

/* Stores last return status */
static int laststatus;
/* Sigaction structures for signal handling */
static struct sigaction sigact, sigdfl;
/* All signals used to be registered/unregistered */
static int sigregistered[] = {SIGCHLD, SIGINT, SIGTSTP, SIGTTIN, SIGTTOU, -1};

/* Register signals */
static void register_signals (struct sigaction* sig);
/* Signal handler */
static void sig_handler (int signo);
/* Reap zombies */
static void grim_reaper (void);

/****************/
/* Redirections */
/****************/

/* Flags for redirection modes */
/* O_CLOEXEC is defined in POSIX.1-2008, _X_OPEN_SOURCE >= 700 */
#define O_OUT O_WRONLY | O_CREAT | O_CLOEXEC | O_TRUNC
#define O_IN O_RDONLY | O_CLOEXEC
#define S_MODE 0666

/* Launch jobs as pipelines */
static int lay_pipeline (const Expression* e, int options);
/* Launch jobs with redirections */
static int lay_redirection (const Expression* e, int options);

/****************/
/* Command exec */
/****************/

/* Job control commands (fg & bg) */
static int cmd_jobctrl (char* job_cmd, int bg);
/* Internal commands */
static int internal_cmd (char* cmd, char** argv);
/* Launch a "SIMPLE" command (node) */
static int start_cmd (char* cmd, char** argv, int options, int notify);

/**********************/
/* Expression handler */
/**********************/

/* Recursive handler */
static int expression_handler (const Expression* e, int options, int notify);

/*======================*/
/* BEGIN IMPLEMENTATION */
/*======================*/

/***************/
/* Job control */
/***************/

/* Initialize shell */
static int
init_shell (void)
{
    /* Prepare our handlers */
    sigemptyset (&sigact.sa_mask);
    /* Prevent I/O primitives from terminating earlier with EINTR */
    sigact.sa_flags   = SA_RESTART;
    sigact.sa_handler = sig_handler;

    /* Prepare a default for later */
    sigemptyset (&sigdfl.sa_mask);
    sigdfl.sa_flags   = 0;
    sigdfl.sa_handler = SIG_DFL;

    /* Register signals */
    register_signals (&sigact);

    /* Clear all jobs*/
    memset (job_list, 0, sizeof job_list);

    /* Get our own group */
    shpid = getpid ();
    if (setpgid (shpid, shpid) < 0)
        return 0;

    /* Get control of the terminal */
    if (tcsetpgrp (0, shpid) < 0)
        /* If we can't get it, we are not in interactive mode */
        interactive = 0;

    /* Shell initialization done */
    init_flag = 1;

    return 0;
}

/* Register a new job */
static job_t*
register_job (pid_t pid, pid_t pgid, int background, char* cmd)
{
    /* Find a free job in the array */
    for (int i = 0; i < MAXJOBS; ++i)
        if (job_list[i].pid == 0)
        {
            job_list[i].jid        = i;
            job_list[i].pid        = pid;
            job_list[i].pgid       = pgid;
            job_list[i].background = background;
            job_list[i].state      = JRUNNING;

            /* Store the command argument */
            if (cmd)
                for (int j = 0; j < CMDBUFSZ; ++j)
                {
                    job_list[i].cmd[j] = cmd[j];
                    if (cmd[j] == '\0')
                        break;
                }

            return &job_list[i];
        }

    /* Cannot find a free job to use */
    return NULL;
}

/* Launch a job  */
static void
launch_job (job_t* job, int notify)
{
    assert (job);

    /* Always start in stopped mode */
    job->state = JSTOPPED;

    /* Assign its own group */
    setpgid (job->pid, job->pid);

    if (job->background == JFG)
        send_to_foreground (job);
    else
    {
        send_to_background (job);
        if (notify)
            fprintf (stdout, "[%d] %d\n", job->jid, job->pid);
    }
}

/* Unregister job */
static void
unregister_job (job_t* job)
{
    assert (job);
    job->pid     = 0;
    job->jid     = 0;
    job->status  = 0;
    job->termsig = 0;
}

/* Remove old jobs */
static void
remove_old_jobs (int notify)
{
    for (int i = 0; i < MAXJOBS; ++i)
        if (job_list[i].pid != 0 && job_list[i].state == JDONE)
        {
            /* We do not want to notify for foreground jobs */
            if (notify && job_list[i].background == JBG)
                display_job (&job_list[i]);
            unregister_job (&job_list[i]);
        }
}

/* Find a job from a pid */
static job_t*
find_job (pid_t pid)
{
    for (int i = 0; i < MAXJOBS; ++i)
        if (job_list[i].pid == pid)
            return &job_list[i];
    return NULL;
}

/* Suspend a job using TSTP */
static void
suspend_job (job_t* job)
{
    assert (job);

    /* Send Terminal Stop to job */
    if (kill (job->pid, SIGTSTP) < 0)
        perror ("Unable to send TSTP");

    job->state      = JSTOPPED;
    job->background = JBG;

    last_job = job;
}

/* Send a job to foreground */
static void
send_to_foreground (job_t* job)
{
    assert (job);

    /* Set default handler to all signals */
    /* This is to prevent re-entering and f'ing up some apps */
    register_signals (&sigdfl);

    /* Give the terminal back to the job */
    if (interactive)
        tcsetpgrp (0, job->pid);

    /* Set current foreground job */
    fg_job = job;

    /* Send SIGCONT if in stopped state */
    if (job->state == JSTOPPED)
        if (kill (job->pid, SIGCONT) < 0)
            fprintf (stderr,
                     "Unable to send continue to job %d: %s\n",
                     job->jid,
                     strerror (errno));

    /* Wait for job to finish or stopped by user */
    if (waitpid (job->pid, &job->status, WUNTRACED) < 0)
        perror ("Wait foregroud job");

    /* Set exit status to job */
    set_status_job (job, job->status);

    /* Re-register signal */
    register_signals (&sigact);

    /* Give back terminal to shell */
    if (interactive)
        tcsetpgrp (0, shpid);
}

/* Send a job to background (and resume it) */
static void
send_to_background (job_t* job)
{
    assert (job);

    /* Send SIGCONT if in stopped state */
    if (job->state == JSTOPPED)
        if (kill (job->pid, SIGCONT) < 0)
            fprintf (stderr,
                     "Unable to send continue to job %d: %s\n",
                     job->jid,
                     strerror (errno));

    job->state = JRUNNING;
    last_job   = job;
}

/* Display a job */
static void
display_job (const job_t* job)
{
    assert (job);

    char* strstate = "Running";
    if (job->state == JDONE)
        strstate = "Done";
    else if (job->state == JSTOPPED)
        strstate = "Suspended";

    fprintf (stdout,
             "[%d]+ %s\t%s\tPID: %d",
             job->jid,
             strstate,
             job->cmd,
             job->pid);
    if (job->state == JDONE)
        if (job->termsig)
            fprintf (stdout, "\tTerminated with signal %d\n", job->termsig);
        else
            fprintf (stdout, "\tExit %d\n", job->status);
    else
        fprintf (stdout, "\n");
}

/*******************
 * Signal Handling *
 *******************/

/* Register signals used by the program */
static void
register_signals (struct sigaction* sig)
{
    assert (sig);

    /* All signals are in a list terminated by -1 */
    for (int* s = sigregistered; *s != -1; s++)
        if (sigaction (*s, sig, NULL) != 0)
            goto err;

    return;

err:
    perror ("Unable to register signals");
}

/* Signal handler */
static void
sig_handler (int signo)
{
    job_t* jb;
    switch (signo)
    {
        /* Reap child */
        case SIGCHLD:
            grim_reaper ();
            break;

        /* Send INT to foreground job */
        case SIGINT:
            if (!fg_job)
                break;
            if (kill (fg_job->pid, SIGINT) < 0)
                perror ("Unable to send SIGINT to foreground process");
            break;

        /* Suspend current foreground job */
        case SIGTSTP:
            if (fg_job)
                suspend_job (fg_job);
            break;

        /* Get back control of the terminal */
        // TODO: Sequence with stdout being suspended
        case SIGTTIN:
        case SIGTTOU:
            tcsetpgrp (0, shpid);
            break;
    }
}

/* Change status of jobs */
static inline void
set_status_job (job_t* job, int wstatus)
{
    assert (job);

    if (WIFEXITED (wstatus))
    {
        job->status = WEXITSTATUS (wstatus);
        job->state  = JDONE;
    }
    else if (WIFSTOPPED (wstatus))
    {
        job->status = 0;
        job->state  = JSTOPPED;
    }
    else if (WIFSIGNALED (wstatus))
    {
        job->state   = JDONE;
        job->termsig = WTERMSIG (wstatus);
    }
}

/* Reap zombie processes */
static void
grim_reaper (void)
{
    /* Iterator */
    job_t* job = job_list;

    int wstatus;
    pid_t pid;

    for (int i = 0; i < MAXJOBS; ++i, job = &job_list[i])
        if (job->pid > 0) /* Only reap job that actually exist */
            if (waitpid (job->pid, &wstatus, WUNTRACED | WCONTINUED | WNOHANG)
                > 0)
                set_status_job (job, wstatus);
            else if (kill (job->pid, 0) == -1)
                /* If the job "exists" but the linked process does not, remove it */
                if (errno == ESRCH)
                    unregister_job (job);
}

/***********************
 * Prepare for command *
 ***********************/

/* Create a pipe and launch attached jobs */
// TODO: Fix pipe with sequences not working correctly
static int
lay_pipeline (const Expression* e, int options)
{
    int wstatus;
    int pipefd[2];

    /* O_CLOEXEC closes the pipe's file descriptors on exec
     This prevents the pipe from being opened and having a background process
     never ending */
    if (pipe2 (pipefd, O_CLOEXEC) == -1)
        goto err;

    int out = dup (STDOUT_FILENO);
    int in  = dup (STDIN_FILENO);

    if (out == -1 || in == -1)
        goto err;
    if (dup2 (pipefd[0], STDIN_FILENO) == -1)
        goto err;
    if (close (pipefd[0]) == -1)
        goto err;

    /* Assign a job for each right side of a pipe (read hand) */
    wstatus = expression_handler (e->droite, JBG, 0);

    if (dup2 (in, STDIN_FILENO) == -1)
        goto err;
    if (dup2 (pipefd[1], STDOUT_FILENO) == -1)
        goto err;
    if (close (pipefd[1]) == -1)
        goto err;

    /* Start in foreground the most-left command */
    wstatus = expression_handler (e->gauche, options, 0);

    if (dup2 (out, STDOUT_FILENO) == -1)
        goto err;

    /* Close renmant pipes, if more than one pipe */
    if (close (out) < 0 || close (in) < 0)
        goto err;

    return wstatus;

err:
    perror ("Unable to set pipe");
    return -1;
}

/* Redirect i/o to a file, returns its file descriptor */
static int
lay_redirection (const Expression* e, int options)
{
    int flags = O_OUT;
    int fd;
    int wstatus;

    /* Save current fd */
    int out = dup (STDOUT_FILENO);
    int in  = dup (STDIN_FILENO);
    int err = dup (STDERR_FILENO);
    if (out == -1 || in == -1 || err == -1)
        goto err;

    /* Set flags */
    if (e->type == REDIRECTION_A)
        flags = (flags & ~O_TRUNC) | O_APPEND;
    if (e->type == REDIRECTION_I)
        flags = O_IN;

    /* Open file for redirection */
    if ((fd = open (*e->arguments, flags, S_MODE)) == -1)
        goto err;

    /* Set redirection for next command */
    switch (e->type)
    {
        case REDIRECTION_I:
            if (dup2 (fd, STDIN_FILENO) == -1)
                goto err2;
            break;

        case REDIRECTION_E:
            if (dup2 (fd, STDERR_FILENO) == -1)
                goto err2;
            break;

        case REDIRECTION_EO:
            if (dup2 (fd, STDERR_FILENO) == -1)
                goto err2;
        case REDIRECTION_A:
        case REDIRECTION_O:
            if (dup2 (fd, STDOUT_FILENO) == -1)
                goto err2;
            break;
    }

    /* Launch next command */
    wstatus = expression_handler (e->gauche, options, 0);

    if (dup2 (in, STDIN_FILENO) == -1 || close (in) == -1)
        goto err2;
    if (dup2 (out, STDOUT_FILENO) == -1 || close (out) == -1)
        goto err3;
    if (dup2 (err, STDERR_FILENO) == -1 || close (err) == -1)
        goto err4;
    if (close (fd) == -1)
        goto err;

    return wstatus;

err4:
    close (out);
err3:
    close (in);
err2:
    close (fd);
err:
    fprintf (stderr, "%s: %s\n", *e->arguments, strerror (errno));
    return -1;
}

/********************
 * Command handling *
 ********************/

/* Start a sequence */
static int
start_sequence (const Expression* e, int options, int notify)
{
    /* Create a job if it's a background sequence */
    if (options == JBG)
    {
        pid_t pid = fork ();
        if (pid < 0)
            perror ("Unable to fork");

        if (!pid)
        {
            register_signals (&sigdfl);
            if (setpgid (0, 0) < 0)
                perror ("Unable to get pgid");
            int wstatus = start_sequence (e, JFG, notify);
            STATUS (wstatus);
            exit (wstatus);
        }

        job_t* job = register_job (pid, pid, JBG, "Sequence");

        /* No more job space available */
        if (!job)
        {
            fprintf (stderr,
                     "Unable to register a new job, terminate some jobs first "
                     "(max: %d)\n",
                     MAXJOBS);
            return INTERNSTATUS + 1;
        }

        launch_job (job, notify);
        return INTERNSTATUS;
    }

    int wstatus;

    wstatus = expression_handler (e->gauche, options, 0);
    STATUS (wstatus);

    switch (e->type)
    {
        case SEQUENCE_ET:
            if (wstatus)
                break;
            wstatus = expression_handler (e->droite, options, 0);
            break;

        case SEQUENCE_OU:
            if (!wstatus)
                break;
        case SEQUENCE:
            wstatus = expression_handler (e->droite, options, 0);
    }

    /* Convert status to correct one */
    STATUS (wstatus);

    return wstatus;
}

/* Internal commands "fg" and "bg" */
static int
cmd_jobctrl (char* job_cmd, int bg)
{
    job_t* tmp;

    /* Find by name */
    if (job_cmd)
    {
        for (int i = 0; i < MAXJOBS; ++i)
            if (strcmp (job_list[i].cmd, job_cmd) == 0 && job_list[i].pid != 0
                && job_list[i].pid != 0)
            {
                tmp = &job_list[i];

                if (bg == JBG && tmp->state == JRUNNING)
                {
                    fprintf (
                        stderr, "%s: job already in background\n", tmp->cmd);
                    return 1;
                }

                fprintf (stdout, "[%d]+ Resumed\t%s\n", tmp->jid, tmp->cmd);

                if (bg == JBG)
                    send_to_background (tmp);
                else
                    send_to_foreground (tmp);

                return 0;
            }

        fprintf (stderr,
                 "%s: job not found: %s\n",
                 bg == JBG ? "bg" : "fg",
                 job_cmd);

        return 1;
    }

    /* Find by last job */

    /* Find any job if there are none in last_job or it's already done*/
    if (!last_job || last_job->state == JDONE)
    {
        for (int i = 0; i < MAXJOBS; ++i)
        {
            if (job_list[i].pid != 0 && job_list[i].state != JDONE)
            {
                last_job = &job_list[i];
                break;
            }
        }
        /* No jobs to be fg'd */
        if (!last_job || last_job->state == JDONE)
        {
            fprintf (stderr, "%s: no job to resume\n", bg ? "bg" : "fg");
            return 1;
        }
    }

    tmp = last_job;

    /* Check if there is not a more recent job (pid higher) */
    for (int i = 0; i < MAXJOBS; ++i)
        if (job_list[i].pid != 0 && job_list[i].state != JDONE
            && job_list[i].pid >= tmp->pid)
            tmp = &job_list[i];

    if (bg == JBG && tmp->state == JRUNNING)
    {
        fprintf (stderr, "%s: job already in background\n", tmp->cmd);
        return 1;
    }

    fprintf (stdout, "[%d]+ Resumed\t%s\n", tmp->jid, tmp->cmd);
    if (bg == JBG)
        send_to_background (tmp);
    else
        send_to_foreground (tmp);

    return 0;
}

/* Handles internal commands */
static int
internal_cmd (char* cmd, char** argv)
{
    char* arg;
    switch (hash_cmd (cmd))
    {
        /* Exit terminal */
        case EXIT:
            EndOfFile ();
            return 0;

        /* Print its arguments or last exit code */
        case ECHO:
            if (argv[1] == NULL)
                return 0;
            if (hash_cmd (*(argv + 1)) == ECHO_STATUS)
            {
                printf ("%d ", laststatus);
                ++argv;
            }
            while ((arg = *++argv))
            {
                printf ("%s", arg);
                if (*(argv + 1))
                    printf (" ");
            }
            printf ("\n");
            return 0;

        /* Change working directory */
        case CD:
            /* TODO: if no argument go to ~ */
            if (argv[1] == NULL)
                return 0;
            if (chdir (*(argv + 1)) == 0)
                return 0;
            fprintf (stderr,
                     "Unable to change directory: %s (%s)\n",
                     strerror (errno),
                     *argv + 1);
            return 1;

        /* Display help */
        case HELP:
            dipslay_help ();
            return 0;

        /* Print a "hash" */
        case HASH:
            if (argv[1] == NULL)
            {
                fprintf (stderr, "hash: no argument to hash\n");
                return 1;
            }
            printf ("%x\n", hash_cmd (argv[1]));
            return 0;

        /* List all jobs */
        case JOBS:
            for (int i = 0; i < MAXJOBS; ++i)
                if (job_list[i].pid != 0)
                    display_job (&job_list[i]);
            return 0;

        /* Send to foreground */
        case CFG:
            return cmd_jobctrl (argv[1], JFG);
        /* Send to background */
        case CBG:
            return cmd_jobctrl (argv[1], JBG);
    }

    /* No internal command foun */
    return -1;
}

/* Start the command accordingly */
static int
start_cmd (char* cmd, char** argv, int options, int notify)
{
    int wstatus;

    /* Check if the command is an internal one */
    wstatus = internal_cmd (cmd, argv);
    if (wstatus != -1)
        return wstatus;

    pid_t pid = fork ();
    if (!pid)
    {
        /* Re-register default signal actions */
        register_signals (&sigdfl);

        /* Set it in its own group */
        if (setpgid (0, 0) < 0)
            perror ("Unable to get its own group");

        execvp (cmd, argv);

        fprintf (stderr, "%s: command not found\n", cmd);
        exit (1);
    }

    /* Register a new job */
    job_t* job = register_job (pid, pid, options, cmd);

    /* No more job space available */
    if (!job)
    {
        fprintf (stderr,
                 "Unable to register a new job, terminate some jobs first "
                 "(max: %d)\n",
                 MAXJOBS);
        return INTERNSTATUS + 1;
    }

    /* launch the job */
    launch_job (job, notify);

    /* If job is in foreground wait and return exit status */
    if (options == JFG)
        return job->status;
    else
        return INTERNSTATUS;
}

/***********************
 * Expression handling *
 ***********************/

/* Handle the expression */
static int
expression_handler (const Expression* e, int options, int notify)
{
    int wstatus;

    if (e->type >= REDIRECTION_I)
        return lay_redirection (e, options);

    switch (e->type)
    {
        case VIDE:
            return INTERNSTATUS;

        case SEQUENCE:
        case SEQUENCE_ET:
        case SEQUENCE_OU:
            return start_sequence (e, options, notify);
        case PIPE:
            return lay_pipeline (e, options);
        case BG:
            return expression_handler (e->gauche, JBG, notify);
        case SIMPLE:
            return start_cmd (*e->arguments, e->arguments, options, notify);
    }

    /* Unexpected */
    fprintf (stderr, "Unexpected error.\n");
    return INTERNSTATUS + 1;
}

/* Entry Point */
int
evaluer_expr (Expression* e)
{
    jmp_buf env;
    int r = setjmp (env);
    if (r > 1)
    {
        fprintf (stderr, "Unable to init shell correctly, quiting...\n");
        exit (EXIT_FAILURE);
    }

    /* Register signals if needed */
    /* Try at least a second time */
    if (!init_flag)
        if (init_shell () < 0)
            longjmp (env, r + 1);

    /* Always foreground by default */
    /* If interactive, then notify for jobs */
    int wstatus = expression_handler (e, JFG, interactive);

    /* Reap old jobs */
    grim_reaper ();

    /* Convert wstatus to correct status */
    STATUS (wstatus);

    /* If the last foreground job had an error, prioritize it */
    if (fg_job && fg_job->status != 0)
        wstatus = fg_job->status;

    /* Update laststatus for %? */
    laststatus = wstatus;

    /* Notify if something bad happened to the last foreground job */
    if (interactive && fg_job)
        switch (fg_job->termsig)
        {
            case SIGSEGV:
                fprintf (stderr, "%s: Segmentation fault.\n", fg_job->cmd);
                break;
            case SIGKILL:
            case SIGTERM:
                fprintf (stderr, "%s: Terminated.\n", fg_job->cmd);
                break;
        }

    /* Unregister old jobs and notify if interactive */
    remove_old_jobs (interactive);

    /* Reset foreground job */
    fg_job = NULL;

    return wstatus;
}
