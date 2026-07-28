#include "curl.hpp"

#include "log.hpp"

#include <cstdlib>
#include <sstream>
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Spawn an external curl, read stdout into out and return its exit code.
// Keeping TLS outside Steam avoids loading another SSL stack into its process.
int Curl::getString(const char* url, std::string& out)
{
	g_pLog->debug("Curl::getString(%s)\n", url);
	out.clear();

	int pipefd[2];

	if (pipe(pipefd) == -1)
	{
		g_pLog->debug("Failed to create pipe!\n");
		return 1;
	}

	g_pLog->debug("Created pipe %i : %i\n", pipefd[0], pipefd[1]);

	const char* args[] =
	{
		"curl",
		"--silent",
		"--show-error",
		"--fail",
		"--location",
		"--connect-timeout", "15",
		"--max-time", "30",
		url,
		nullptr
	};

	const pid_t pid = fork();
	if (pid == -1)
	{
		close(pipefd[0]);
		close(pipefd[1]);
		g_pLog->debug("Failed to fork!\n");
		return 1;
	}

	if (pid == 0)
	{
		if (dup2(pipefd[1], STDOUT_FILENO) == -1)
		{
			g_pLog->debug("Failed to dup2!\n");
			exit(1);
		}

		//No need for reading
		close(pipefd[0]);
		close(pipefd[1]);

		// Keep HOME, proxy, DNS and certificate settings from the user's
		// session, but never propagate Steam's injection environment into an
		// unrelated helper process.
		unsetenv("LD_AUDIT");
		unsetenv("LD_PRELOAD");
		unsetenv("LD_LIBRARY_PATH");

		constexpr static const char* candidates[] =
		{
			"/run/current-system/sw/bin/curl",
			"/usr/bin/curl",
			"/bin/curl"
		};
		for (const char* candidate : candidates)
			execv(candidate, const_cast<char**>(args));

		dprintf(STDERR_FILENO, "SLSsteam: failed to execute curl: %s\n",
		        std::strerror(errno));
		_exit(127);
	}

	//No need for writing
	close(pipefd[1]);

	g_pLog->debug("Child PID %i\n", pid);

	std::ostringstream bufSS;
	char buf[128];
	int numRead;

	while((numRead = read(pipefd[0], buf, sizeof(buf))) > 0)
	{
		bufSS << std::string(buf, numRead);
	}

	int status;
	if(waitpid(pid, &status, 0) == -1)
	{
		return 1;
	}

	if(!WIFEXITED(status))
	{
		return 1;
	}

	status = WEXITSTATUS(status);

	g_pLog->debug("Exit Status: %i\n", status);

	out = bufSS.str();

	return status;
}
