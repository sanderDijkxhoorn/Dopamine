#include "jbserver_global.h"
#include <libjailbreak/info.h>
#include <sandbox.h>
#include <libproc.h>
#include <libproc_private.h>

#include <libjailbreak/signatures.h>
#include <libjailbreak/trustcache.h>
#include <libjailbreak/kernel.h>
#include <libjailbreak/util.h>
#include <libjailbreak/primitives.h>
#include <libjailbreak/codesign.h>

extern bool stringStartsWith(const char *str, const char* prefix);
extern bool stringEndsWith(const char* str, const char* suffix);

static bool systemwide_domain_allowed(audit_token_t clientToken)
{
	return true;
}

static int systemwide_get_jbroot(char **rootPathOut)
{
	*rootPathOut = strdup(jbinfo(rootPath));
	return 0;
}

static int systemwide_get_boot_uuid(char **bootUUIDOut)
{
	const char *launchdUUID = getenv("LAUNCHD_UUID");
	*bootUUIDOut = launchdUUID ? strdup(launchdUUID) : NULL;
	return 0;
}

static int trust_file(const char *filePath, const char *dlopenCallerImagePath, const char *dlopenCallerExecutablePath)
{
	// Shared logic between client and server, implemented in client
	// This should essentially mean these files never reach us in the first place
	// But you know, never trust the client :D
	extern bool can_skip_trusting_file(const char *filePath, bool isLibrary, bool isClient);

	if (can_skip_trusting_file(filePath, (bool)dlopenCallerExecutablePath, false)) return -1;

	cdhash_t *cdhashes = NULL;
	uint32_t cdhashesCount = 0;
	macho_collect_untrusted_cdhashes(filePath, dlopenCallerImagePath, dlopenCallerExecutablePath, &cdhashes, &cdhashesCount);
	if (cdhashes && cdhashesCount > 0) {
		jb_trustcache_add_cdhashes(cdhashes, cdhashesCount);
		free(cdhashes);
	}
	return 0;
}

// Not static because launchd will directly call this from it's posix_spawn hook
int systemwide_trust_binary(const char *binaryPath)
{
	return trust_file(binaryPath, NULL, NULL);
}

static int systemwide_trust_library(audit_token_t *processToken, const char *libraryPath, const char *callerLibraryPath)
{
	// Fetch process info
	pid_t pid = audit_token_to_pid(*processToken);
	char callerPath[4*MAXPATHLEN];
	if (proc_pidpath(pid, callerPath, sizeof(callerPath)) < 0) {
		return -1;
	}

	// When trusting a library that's dlopened at runtime, we need to pass the caller path
	// This is to support dlopen("@executable_path/whatever", RTLD_NOW) and stuff like that
	// (Yes that is a thing >.<)
	// Also we need to pass the path of the image that called dlopen due to @loader_path, sigh...
	return trust_file(libraryPath, callerLibraryPath, callerPath);
}

static int systemwide_process_checkin(audit_token_t *processToken, char **rootPathOut, char **bootUUIDOut, char **sandboxExtensionsOut, bool *fullyDebuggedOut)
{
	// Fetch process info
	pid_t pid = audit_token_to_pid(*processToken);
	uint64_t proc = proc_find(pid);
	char procPath[4*MAXPATHLEN];
	if (proc_pidpath(pid, procPath, sizeof(procPath)) < 0) {
		return -1;
	}

	// Get jbroot and boot uuid
	systemwide_get_jbroot(rootPathOut);
	systemwide_get_boot_uuid(bootUUIDOut);

	// Generate sandbox extensions for the requesting process

	// transitd needs to write to /var/jb/var because rootlesshooks make it use that path instead of /var
	bool writeAllowed = !strcmp(procPath, "/usr/libexec/transitd");

	char *readWriteExtension = NULL;
	if (writeAllowed) {
		readWriteExtension = sandbox_extension_issue_file_to_process("com.apple.app-sandbox.read-write", JBRootPath(""), 0, *processToken);
	}
	else {
		readWriteExtension = sandbox_extension_issue_file_to_process("com.apple.app-sandbox.read", JBRootPath(""), 0, *processToken);
	}
	char *execExtension = sandbox_extension_issue_file_to_process("com.apple.sandbox.executable", JBRootPath(""), 0, *processToken);
	if (readWriteExtension && execExtension) {
		char extensionBuf[strlen(readWriteExtension) + 1 + strlen(execExtension) + 1];
		strcpy(extensionBuf, readWriteExtension);
		strcat(extensionBuf, "|");
		strcat(extensionBuf, execExtension);
		*sandboxExtensionsOut = strdup(extensionBuf);
	}
	if (readWriteExtension) free(readWriteExtension);
	if (execExtension) free(execExtension);

	bool fullyDebugged = false;
	if (stringStartsWith(procPath, "/private/var/containers/Bundle/Application") || stringStartsWith(procPath, JBRootPath("/Applications"))) {
		// This is an app
		// Enable CS_DEBUGGED based on user preference
		if (jbsetting(markAppsAsDebugged)) {
			fullyDebugged = true;
		}
	}
	*fullyDebuggedOut = fullyDebugged;

	// Allow invalid pages
	cs_allow_invalid(proc, fullyDebugged);

	// Fix setuid
	struct stat sb;
	if (stat(procPath, &sb) == 0) {
		if (S_ISREG(sb.st_mode) && (sb.st_mode & (S_ISUID | S_ISGID))) {
			uint64_t ucred = proc_ucred(proc);
			if ((sb.st_mode & (S_ISUID))) {
				kwrite32(proc + koffsetof(proc, svuid), sb.st_uid);
				kwrite32(ucred + koffsetof(ucred, svuid), sb.st_uid);
				kwrite32(ucred + koffsetof(ucred, uid), sb.st_uid);
			}
			if ((sb.st_mode & (S_ISGID))) {
				kwrite32(proc + koffsetof(proc, svgid), sb.st_gid);
				kwrite32(ucred + koffsetof(ucred, svgid), sb.st_gid);
				kwrite32(ucred + koffsetof(ucred, groups), sb.st_gid);
			}
			uint32_t flag = kread32(proc + koffsetof(proc, flag));
			if ((flag & P_SUGID) != 0) {
				flag &= ~P_SUGID;
				kwrite32(proc + koffsetof(proc, flag), flag);
			}
		}
	}

	// In iOS 16+ there is a super annoying security feature called Protobox
	// Amongst other things, it allows for a process to have a syscall mask
	// If a process calls a syscall it's not allowed to call, it immediately crashes
	// Because for tweaks and hooking this is unacceptable, we update these masks to be 1 for all syscalls on all processes
	// That will at least get rid of the syscall mask part of Protobox
	if (__builtin_available(iOS 16.0, *)) {
		proc_allow_all_syscalls(proc);
	}

	// For whatever reason after SpringBoard has restarted, AutoFill and other stuff stops working
	// The fix is to always also restart the kbd daemon alongside SpringBoard
	// Seems to be something sandbox related where kbd doesn't have the right extensions until restarted
	if (strcmp(procPath, "/System/Library/CoreServices/SpringBoard.app/SpringBoard") == 0) {
		static bool springboardStartedBefore = false;
		if (!springboardStartedBefore) {
			// Ignore the first SpringBoard launch after userspace reboot
			// This fix only matters when SpringBoard gets restarted during runtime
			springboardStartedBefore = true;
		}
		else {
			dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
				killall("/System/Library/TextInput/kbd", false);
			});
		}
	}
	// For the Dopamine app itself we want to give it a saved uid/gid of 0, unsandbox it and give it CS_PLATFORM_BINARY
	// This is so that the buttons inside it can work when jailbroken, even if the app was not installed by TrollStore
	else if (stringEndsWith(procPath, "/Dopamine.app/Dopamine")) {
		// svuid = 0, svgid = 0
		uint64_t ucred = proc_ucred(proc);
		kwrite32(proc + koffsetof(proc, svuid), 0);
		kwrite32(ucred + koffsetof(ucred, svuid), 0);
		kwrite32(proc + koffsetof(proc, svgid), 0);
		kwrite32(ucred + koffsetof(ucred, svgid), 0);

		// platformize
		proc_csflags_set(proc, CS_PLATFORM_BINARY);
	}

	proc_rele(proc);
	return 0;
}

static int systemwide_fork_fix(audit_token_t *parentToken, uint64_t childPid)
{
	int retval = 3;
	uint64_t parentPid = audit_token_to_pid(*parentToken);
	uint64_t parentProc = proc_find(parentPid);
	uint64_t childProc = proc_find(childPid);

	if (childProc && parentProc) {
		retval = 2;
		// Safety check to ensure we are actually coming from fork
		if (kread_ptr(childProc + koffsetof(proc, pptr)) == parentProc) {
			cs_allow_invalid(childProc, false);

			uint64_t childTask  = proc_task(childProc);
			uint64_t childVmMap = kread_ptr(childTask + koffsetof(task, map));

			uint64_t parentTask  = proc_task(parentProc);
			uint64_t parentVmMap = kread_ptr(parentTask + koffsetof(task, map));

			uint64_t parentHeader     = kread_ptr(parentVmMap  + koffsetof(vm_map, hdr));
			uint64_t parentEntry      = kread_ptr(parentHeader + koffsetof(vm_map_header, links) + koffsetof(vm_map_links, next));

			uint64_t childHeader     = kread_ptr(childVmMap + koffsetof(vm_map, hdr));
			uint64_t childEntry      = kread_ptr(childHeader + koffsetof(vm_map_header, links) + koffsetof(vm_map_links, next));

			uint64_t childFirstEntry = childEntry, parentFirstEntry = parentEntry;
			do {
				uint64_t childStart  = kread_ptr(childEntry  + koffsetof(vm_map_entry, links) + koffsetof(vm_map_links, min));
				uint64_t childEnd    = kread_ptr(childEntry  + koffsetof(vm_map_entry, links) + koffsetof(vm_map_links, max));
				uint64_t parentStart = kread_ptr(parentEntry + koffsetof(vm_map_entry, links) + koffsetof(vm_map_links, min));
				uint64_t parentEnd   = kread_ptr(parentEntry + koffsetof(vm_map_entry, links) + koffsetof(vm_map_links, max));

				if (parentStart < childStart) {
					parentEntry = kread_ptr(parentEntry + koffsetof(vm_map_entry, links) + koffsetof(vm_map_links, next));
				}
				else if (parentStart > childStart) {
					childEntry = kread_ptr(childEntry + koffsetof(vm_map_entry, links) + koffsetof(vm_map_links, next));
				}
				else {
					uint64_t parentFlags = kread64(parentEntry + koffsetof(vm_map_entry, flags));
					uint64_t childFlags  = kread64(childEntry  + koffsetof(vm_map_entry, flags));

					uint8_t parentProt = VM_FLAGS_GET_PROT(parentFlags), parentMaxProt = VM_FLAGS_GET_MAXPROT(parentFlags);
					uint8_t childProt =  VM_FLAGS_GET_PROT(childFlags),  childMaxProt  = VM_FLAGS_GET_MAXPROT(childFlags);

					if (parentProt != childProt || parentMaxProt != childMaxProt) {
						VM_FLAGS_SET_PROT(childFlags, parentProt);
						VM_FLAGS_SET_MAXPROT(childFlags, parentMaxProt);
						kwrite64(childEntry + koffsetof(vm_map_entry, flags), childFlags);
					}

					parentEntry = kread_ptr(parentEntry + koffsetof(vm_map_entry, links) + koffsetof(vm_map_links, next));
					childEntry = kread_ptr(childEntry + koffsetof(vm_map_entry, links) + koffsetof(vm_map_links, next));
				}
			} while (parentEntry != 0 && childEntry != 0 && parentEntry != parentFirstEntry && childEntry != childFirstEntry);
			retval = 0;
		}
	}
	if (childProc)  proc_rele(childProc);
	if (parentProc) proc_rele(parentProc);

	return 0;
}

static int systemwide_cs_revalidate(audit_token_t *callerToken)
{
	uint64_t callerPid = audit_token_to_pid(*callerToken);
	if (callerPid > 0) {
		uint64_t callerProc = proc_find(callerPid);
		if (callerProc) {
			proc_csflags_set(callerProc, CS_VALID);
			return 0;
		}
	}
	return -1;
}

struct jbserver_domain gSystemwideDomain = {
	.permissionHandler = systemwide_domain_allowed,
	.actions = {
		// JBS_SYSTEMWIDE_GET_JBROOT
		{
			.handler = systemwide_get_jbroot,
			.args = (jbserver_arg[]){
				{ .name = "root-path", .type = JBS_TYPE_STRING, .out = true },
				{ 0 },
			},
		},
		// JBS_SYSTEMWIDE_GET_BOOT_UUID
		{
			.handler = systemwide_get_boot_uuid,
			.args = (jbserver_arg[]){
				{ .name = "boot-uuid", .type = JBS_TYPE_STRING, .out = true },
				{ 0 },
			},
		},
		// JBS_SYSTEMWIDE_TRUST_BINARY
		{
			.handler = systemwide_trust_binary,
			.args = (jbserver_arg[]){
				{ .name = "binary-path", .type = JBS_TYPE_STRING, .out = false },
				{ 0 },
			},
		},
		// JBS_SYSTEMWIDE_TRUST_LIBRARY
		{
			.handler = systemwide_trust_library,
			.args = (jbserver_arg[]){
				{ .name = "caller-token", .type = JBS_TYPE_CALLER_TOKEN, .out = false },
				{ .name = "library-path", .type = JBS_TYPE_STRING, .out = false },
				{ .name = "caller-library-path", .type = JBS_TYPE_STRING, .out = false },
				{ 0 },
			},
		},
		// JBS_SYSTEMWIDE_PROCESS_CHECKIN
		{
			.handler = systemwide_process_checkin,
			.args = (jbserver_arg[]) {
				{ .name = "caller-token", .type = JBS_TYPE_CALLER_TOKEN, .out = false },
				{ .name = "root-path", .type = JBS_TYPE_STRING, .out = true },
				{ .name = "boot-uuid", .type = JBS_TYPE_STRING, .out = true },
				{ .name = "sandbox-extensions", .type = JBS_TYPE_STRING, .out = true },
				{ .name = "fully-debugged", .type = JBS_TYPE_BOOL, .out = true },
				{ 0 },
			},
		},
		// JBS_SYSTEMWIDE_FORK_FIX
		{
			.handler = systemwide_fork_fix,
			.args = (jbserver_arg[]) {
				{ .name = "caller-token", .type = JBS_TYPE_CALLER_TOKEN, .out = false },
				{ .name = "child-pid", .type = JBS_TYPE_UINT64, .out = false },
				{ 0 },
			},
		},
		// JBS_SYSTEMWIDE_CS_REVALIDATE
		{
			.handler = systemwide_cs_revalidate,
			.args = (jbserver_arg[]) {
				{ .name = "caller-token", .type = JBS_TYPE_CALLER_TOKEN, .out = false },
				{ 0 },
			},
		},
		{ 0 },
	},
};