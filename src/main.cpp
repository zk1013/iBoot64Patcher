//
//  main.cpp
//  src
//
//  Created by tihmstar on 27.09.19.
//  Copyright © 2019 tihmstar. All rights reserved.
//

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <libpatchfinder/machopatchfinder32.hpp>
#include <libpatchfinder/machopatchfinder64.hpp>
#include <libpatchfinder/ibootpatchfinder/ibootpatchfinder32.hpp>
#include <libpatchfinder/ibootpatchfinder/ibootpatchfinder64.hpp>
#include <libpatchfinder/kernelpatchfinder/kernelpatchfinder32.hpp>
#include <libpatchfinder/kernelpatchfinder/kernelpatchfinder64.hpp>

#define HAS_ARG(x,y) (!strcmp(argv[i], x) && (i + y) < argc)

#define addpatch(pp) do {\
    auto p = pp; \
    patches.insert(patches.end(), p.begin(), p.end()); \
} while (0)

#define addloc(pp) do {\
    patches.push_back({pp,NULL,0}); \
} while (0)

using namespace tihmstar::patchfinder;

#define FLAG_UNLOCK_NVRAM (1 << 0)

int main(int argc, const char * argv[]) {
    FILE* fp = nullptr;
    FILE* fp2 = nullptr;
    char* cmd_handler_str = nullptr;
    char* custom_boot_args = nullptr;
    uint64_t cmd_handler_ptr = 0;
    int flags = 0;

    printf("Version: " VERSION_COMMIT_SHA "-" VERSION_COMMIT_COUNT "\n");
    
    if(argc < 3) {
        printf("Usage: %s <iboot_in> <iboot_out> [args]\n", argv[0]);
        printf("\t-b <str>\tApply custom boot args.\n");
        printf("\t-c <cmd> <ptr>\tChange a command handler's pointer (hex).\n");
        printf("\t-n \t\tApply unlock nvram patch.\n");
        return -1;
    }
    
    printf("%s: Starting...\n", __FUNCTION__);
    
    for(int i = 0; i < argc; i++) {
        if(HAS_ARG("-b", 1)) {
            custom_boot_args = (char*) argv[i+1];
        } else if(HAS_ARG("-n", 0)) {
            flags |= FLAG_UNLOCK_NVRAM;
        }else if(HAS_ARG("-c", 2)) {
            cmd_handler_str = (char*) argv[i+1];
            sscanf((char*) argv[i+2], "0x%016llX", &cmd_handler_ptr);
        }
    }
    
    std::vector<patch> patches;
    
    ibootpatchfinder *ibpf = nullptr;
    kernelpatchfinder *kpf = nullptr;
    cleanup([&]{
      safeDelete(ibpf);
      safeDelete(kpf);
    });
    const char *iboot_path = argv[1];
    const char *iboot_patched_path = argv[2];
    struct stat st{0};
    if(stat(iboot_path, &st) < 0) {
      printf("%s: Error getting iBoot size for %s!\n", __FUNCTION__, iboot_path);
      return -1;
    }
    size_t iboot_size = st.st_size;
    try {
      kpf = kernelpatchfinder64::make_kernelpatchfinder64(iboot_path);
    } catch (...) {
      try {
        kpf = kernelpatchfinder32::make_kernelpatchfinder32(iboot_path);
      } catch (...) {
        try {
          ibpf = ibootpatchfinder64::make_ibootpatchfinder64(iboot_path);
        } catch (...) {
          ibpf = ibootpatchfinder32::make_ibootpatchfinder32(iboot_path);
        }
      }
    }
    /* Check to see if the loader has a kernel load routine before trying to apply custom boot args + debug-enabled override. */
    if(ibpf->has_kernel_load()) {
        if(custom_boot_args) {
            try {
                printf("getting get_boot_arg_patch(%s) patch\n",custom_boot_args);
                addpatch(ibpf->get_boot_arg_patch(custom_boot_args));
            } catch (tihmstar::exception &e) {
                printf("%s: Error doing patch_boot_args()! (%s)\n", __FUNCTION__, e.what());
                return -1;
            }
        }
        
        
        /* Only bootloaders with the kernel load routines pass the DeviceTree. */
        try {
            printf("getting get_debug_enabled_patch() patch\n");
            addpatch(ibpf->get_debug_enabled_patch());
        } catch (tihmstar::exception &e) {
            printf("%s: Error doing patch_debug_enabled()! (%s)\n", __FUNCTION__, e.what());
            return -1;
        }
    }
    
    /* Ensure that the loader has a shell. */
    if(ibpf->has_recovery_console()) {
        if (cmd_handler_str && cmd_handler_ptr) {
            try {
                printf("getting get_cmd_handler_patch(%s,0x%016llx) patch\n",cmd_handler_str,cmd_handler_ptr);
                addpatch(ibpf->get_cmd_handler_patch(cmd_handler_str, cmd_handler_ptr));
            } catch (tihmstar::exception &e) {
                printf("%s: Error doing patch_cmd_handler()! (%s)\n", __FUNCTION__, e.what());
                return -1;
            }
        }
        
        if (flags & FLAG_UNLOCK_NVRAM) {
            try {
                printf("getting get_unlock_nvram_patch() patch\n");
                addpatch(ibpf->get_unlock_nvram_patch());
            } catch (tihmstar::exception &e) {
                printf("%s: Error doing get_unlock_nvram_patch()! (%s)\n", __FUNCTION__, e.what());
                return -1;
            }
            try {
                printf("getting get_freshnonce_patch() patch\n");
                addpatch(ibpf->get_freshnonce_patch());
            } catch (tihmstar::exception &e) {
                printf("%s: Error doing get_freshnonce_patch()! (%s)\n", __FUNCTION__, e.what());
                return -1;
            }
        }
    }
    
    /* All loaders have the RSA check. */
    try {
        printf("getting get_sigcheck_patch() patch\n");
        addpatch(ibpf->get_sigcheck_patch());
    } catch (tihmstar::exception &e) {
        printf("%s: Error doing patch_rsa_check()! (%s)\n", __FUNCTION__, e.what());
        return -1;
    }
    
    
    /* Write out the patched file... */
    fp = fopen(iboot_patched_path, "wb+");
    if(!fp) {
        printf("%s: Unable to open %s!\n", __FUNCTION__, iboot_patched_path);
        return -1;
    }
    fp2 = fopen(iboot_path, "rb+");
    if(!fp2) {
        printf("%s: Unable to open %s!\n", __FUNCTION__, iboot_path);
        fflush(fp);
        fclose(fp);
        return -1;
    }
    char *deciboot = (char *)calloc(1, iboot_size);
    size_t ret = fread(deciboot, 1, iboot_size, fp2);
    if(ret != iboot_size) {
      printf("%s: Unable to read iBoot, read size %zu/%zu!\n", __FUNCTION__, ret, iboot_size);
      fflush(fp);
      fclose(fp);
      fflush(fp2);
      fclose(fp2);
      free(deciboot);
      return -1;
    }
    fflush(fp2);
    fclose(fp2);

    for (const auto& p2 : patches) {
      printf("%s: Applying patch=0x%016llx: ", __FUNCTION__, p2._location);
      for (int i=0; i<p2._patchSize; i++) {
        printf("%02x",((uint8_t*)p2._patch)[i]);
      }
      if (p2._patchSize == 4) {
        printf(" 0x%08x",*(uint32_t*)p2._patch);
      } else if (p2._patchSize == 2) {
        printf(" 0x%04x",*(uint16_t*)p2._patch);
      }
      printf("\n");
      auto off = (ibootpatchfinder::loc64_t)(p2._location - ibpf->find_base());
      memcpy(&deciboot[off], p2._patch, p2._patchSize);
    }
    printf("%s: Writing out patched file to %s...\n", __FUNCTION__, iboot_patched_path);
    ret = fwrite(deciboot,1, iboot_size, fp);
    if(ret != iboot_size) {
      printf("%s: Unable to write patched iBoot, wrote size %zu/%zu!\n", __FUNCTION__, ret, iboot_size);
      fflush(fp);
      fclose(fp);
      fflush(fp2);
      fclose(fp2);
      free(deciboot);
      return -1;
    }

    fflush(fp);
    fclose(fp);
    free(deciboot);

    printf("%s: Quitting...\n", __FUNCTION__);
    
    return 0;
}
