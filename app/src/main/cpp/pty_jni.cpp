// Local PTY + shell spawn for the Harn Android terminal.
//
// Who can reach this: the device user of this app.
// Worst input: shell commands as the app UID.
// Failure leak: local process errors. No secrets.

#include <jni.h>

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace {

constexpr const char* kLogTag = "harn-pty";

void setWindowSize(int fd, jint cols, jint rows, jint cellWidthPx, jint cellHeightPx) {
    struct winsize size {};
    size.ws_col = static_cast<unsigned short>(cols);
    size.ws_row = static_cast<unsigned short>(rows);
    size.ws_xpixel = static_cast<unsigned short>(cols * cellWidthPx);
    size.ws_ypixel = static_cast<unsigned short>(rows * cellHeightPx);
    ioctl(fd, TIOCSWINSZ, &size);
}

std::string jstringToUtf8(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return {};
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

void closeQuiet(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

}  // namespace

extern "C" {

JNIEXPORT jintArray JNICALL Java_com_example_harn_terminal_LocalPty_nativeStart(
        JNIEnv* env, jclass, jstring jCwd, jstring jShell, jobjectArray jEnvPairs, jint cols,
        jint rows, jint cellWidthPx, jint cellHeightPx) {
    const int master = posix_openpt(O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (master < 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "posix_openpt failed: %d", errno);
        return nullptr;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "grantpt/unlockpt failed: %d", errno);
        closeQuiet(master);
        return nullptr;
    }

    char* slaveName = ptsname(master);
    if (slaveName == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "ptsname failed: %d", errno);
        closeQuiet(master);
        return nullptr;
    }

    const std::string cwd = jstringToUtf8(env, jCwd);
    const std::string shell = jstringToUtf8(env, jShell);
    const char* shellPath = shell.empty() ? "/system/bin/sh" : shell.c_str();

    std::vector<std::string> envOwned;
    std::vector<const char*> envp;
    if (jEnvPairs != nullptr) {
        const jsize count = env->GetArrayLength(jEnvPairs);
        envOwned.reserve(static_cast<size_t>(count));
        for (jsize i = 0; i < count; i++) {
            auto entry = static_cast<jstring>(env->GetObjectArrayElement(jEnvPairs, i));
            envOwned.push_back(jstringToUtf8(env, entry));
            if (entry != nullptr) {
                env->DeleteLocalRef(entry);
            }
        }
    }
    envp.reserve(envOwned.size() + 1);
    for (const auto& item : envOwned) {
        envp.push_back(item.c_str());
    }
    envp.push_back(nullptr);

    setWindowSize(master, cols, rows, cellWidthPx, cellHeightPx);

    const pid_t pid = fork();
    if (pid < 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "fork failed: %d", errno);
        closeQuiet(master);
        return nullptr;
    }
    if (pid == 0) {
        closeQuiet(master);
        if (setsid() < 0) {
            _exit(127);
        }
        const int slave = open(slaveName, O_RDWR);
        if (slave < 0) {
            _exit(127);
        }
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) {
            close(slave);
        }
        if (!cwd.empty()) {
            chdir(cwd.c_str());
        }
        const char* argv[] = {shellPath, "-i", nullptr};
        execve(shellPath, const_cast<char**>(argv), const_cast<char**>(envp.data()));
        _exit(127);
    }

    jintArray result = env->NewIntArray(2);
    if (result == nullptr) {
        kill(pid, SIGTERM);
        closeQuiet(master);
        return nullptr;
    }
    const jint values[2] = {master, pid};
    env->SetIntArrayRegion(result, 0, 2, values);
    return result;
}

JNIEXPORT void JNICALL Java_com_example_harn_terminal_LocalPty_nativeSetWindowSize(
        JNIEnv*, jclass, jint fd, jint cols, jint rows, jint cellWidthPx, jint cellHeightPx) {
    if (fd >= 0) {
        setWindowSize(fd, cols, rows, cellWidthPx, cellHeightPx);
    }
}

JNIEXPORT void JNICALL Java_com_example_harn_terminal_LocalPty_nativeClose(JNIEnv*, jclass,
                                                                           jint pid) {
    if (pid > 0) {
        kill(pid, SIGHUP);
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, WNOHANG);
        }
    }
}

}  // extern "C"
