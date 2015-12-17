#include "Daemon.h"
#ifndef _WIN32
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "util/Log.h"
#include "util/Filesystem.h"
#include "util/Config.h"

void handle_signal(int sig)
{
    switch (sig)
    {
    case SIGHUP:
        if (Daemon.m_isDaemon == 1)
        {
            static bool first=true;
            if (first)
            {
                first=false;
                return;
            }
        }
        LogPrint("Reloading config...");
        // TODO rewrite ParseConfigFile() to respond to SIGHUP
        LogPrint("Config reloaded");
        break;
    case SIGABRT:
    case SIGTERM:
    case SIGINT:
        Daemon.m_isRunning = 0; // Exit loop
        break;
    }
}

namespace i2p
{
    namespace util
    {
        bool DaemonLinux::Start()
        {
            if (m_isDaemon == 1)
            {
                pid_t pid;
                pid = fork();
                if (pid > 0) // parent
                    ::exit (EXIT_SUCCESS);

                if (pid < 0) // error
                    return false;

                // child
                umask(0);
                int sid = setsid();
                if (sid < 0)
                {
                    LogPrint("Error, could not create process group.");
                    return false;
                }
                std::string d(i2p::util::filesystem::GetDataPath().string()); // make a copy
                chdir(d.c_str());

                // close stdin/stdout/stderr descriptors
                ::close (0);
                ::open ("/dev/null", O_RDWR);
                ::close (1);
                ::open ("/dev/null", O_RDWR);   
                ::close (2);
                ::open ("/dev/null", O_RDWR);
            }

            // Pidfile
            m_pidfile = IsService() ? "/var/run" : i2p::util::filesystem::GetDataPath().string();
            m_pidfile.append("/kovri.pid");
            m_pidFilehandle = open(m_pidfile.c_str(), O_RDWR | O_CREAT, 0600);
            if (m_pidFilehandle == -1)
            {
                LogPrint("Error, could not create pid file (", m_pidfile, ")\nIs an instance already running?");
                return false;
            }
            if (lockf(m_pidFilehandle, F_TLOCK, 0) == -1)
            {
                LogPrint("Error, could not lock pid file (", m_pidfile, ")\nIs an instance already running?");
                return false;
            }
            char pid[10];
            sprintf(pid, "%d\n", getpid());
            write(m_pidFilehandle, pid, strlen(pid));

            // Signal handler
            struct sigaction sa;
            sa.sa_handler = handle_signal;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_RESTART;
            sigaction(SIGHUP, &sa, 0);
            sigaction(SIGABRT, &sa, 0);
            sigaction(SIGTERM, &sa, 0);
            sigaction(SIGINT, &sa, 0);

            return Daemon_Singleton::Start();
        }

        bool DaemonLinux::Stop()
        {
            close(m_pidFilehandle);
            unlink(m_pidfile.c_str());

            return Daemon_Singleton::Stop();
        }

    }
}

#endif
