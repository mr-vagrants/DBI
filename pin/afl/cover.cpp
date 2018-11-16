#include <pin.H>
#include <string>
#include <cstdlib>
#include <iostream>

#define VERSION "0.39"

#ifdef _WIN64
    #define __win__ 1
#elif _WIN32
    #define __win__ 1
#endif

#ifdef __linux__
    #include <sys/shm.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #include <limits.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <signal.h>
    int afl_sync_fd = -1;
    bool write_to_pipe(char *);
#elif __win__
    namespace windows {
        #include <Windows.h>
        bool write_to_pipe(char *);
    }
#endif

// 65536
#define MAP_SIZE    (1 << 16)
#define FORKSRV_FD  198

//  CLI options -----------------------------------------------------------

KNOB<BOOL> Knob_debug(KNOB_MODE_WRITEONCE,  "pintool", "debug", "0", "Enable debug mode");
KNOB<ADDRINT> Knob_entry(KNOB_MODE_WRITEONCE, "pintool", "entry", "0", "start address for coverage signal");
KNOB<ADDRINT> Knob_exit(KNOB_MODE_WRITEONCE, "pintool", "exit", "0", "stop address for coverage signal");

//  Global Vars -----------------------------------------------------------

BOOL coverage_enable = TRUE;
ADDRINT min_addr = 0;
ADDRINT max_addr = 0;
ADDRINT entry_addr = 0;
ADDRINT exit_addr = 0;

unsigned char bitmap[MAP_SIZE];
uint8_t *bitmap_shm = 0;

ADDRINT last_id = 0;

//  inlined functions -----------------------------------------------------

inline ADDRINT valid_addr(ADDRINT addr)
{
    if ( addr >= min_addr && addr <= max_addr )
        return true;

    return false;
}

//  Inserted functions ----------------------------------------------------

VOID fuzzer_synchronization(char *cmd)
{
    #ifdef __win__
    windows::write_to_pipe(cmd);
    #elif __linux__
    write_to_pipe(cmd);
    #endif
}

// Unused currently but could become a fast call in the future once I have tested it more.
VOID TrackBranch(ADDRINT cur_addr)
{
    ADDRINT cur_id = cur_addr - min_addr;

    if (Knob_debug) {
        std::cout << "\nCURADDR:  0x" << cur_addr << std::endl;
        std::cout << "rel_addr: 0x" << (cur_addr - min_addr) << std::endl;
        std::cout << "cur_id:  " << cur_id << std::endl;
        std::cout << "index:  " << ((cur_id ^ last_id) % MAP_SIZE) << std::endl;
    }

    if(coverage_enable)
    {
        if (bitmap_shm != 0){
            bitmap_shm[((cur_id ^ last_id) % MAP_SIZE)]++;
        }
        else {
            bitmap[((cur_id ^ last_id) % MAP_SIZE)]++;
        }
    }
    last_id = cur_id;

    if(entry_addr && entry_addr == cur_id)
    {
        std::cout << "entry" << std::endl;
        coverage_enable = TRUE;
    }
    else if(exit_addr && exit_addr == cur_id)
    {
        std::cout << "exit" << std::endl;
        coverage_enable = FALSE;
        fuzzer_synchronization( (char *) "e" );
    }
}

//  Analysis functions ----------------------------------------------------

VOID Trace(TRACE trace, VOID *v)
{
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
        {
            // make sure it is in a segment we want to instrument!
            if (valid_addr(INS_Address(ins)))
            {
                if (INS_IsBranch(ins)) {
                    // As per afl-as.c we only care about conditional branches (so no JMP instructions)
                    if (INS_HasFallThrough(ins) || INS_IsCall(ins))
                    {
                        if (Knob_debug) {
                            
                            std::cout << "BRACH: 0x" << std::hex << INS_Address(ins) << ":\t" << INS_Disassemble(ins) << std::endl;
                        }

                        // Instrument the code.
                        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)TrackBranch,
                            IARG_INST_PTR,
                            IARG_END);
                    }
                }
            }
        }
    }
}

VOID entry_point(VOID *ptr)
{
    /*  Much like the original instrumentation from AFL we only want to instrument the segments of code
     *  from the actual application and not the link and PIN setup itself.
     *
     *  Inspired by: http://joxeankoret.com/blog/2012/11/04/a-simple-pin-tool-unpacker-for-the-linux-version-of-skype/
     */

    IMG img = APP_ImgHead();
    for(SEC sec= IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec))
    {
        // lets sanity check the exec flag 
        // TODO: the check for .text name might be too much, there could be other executable segments we
        //       need to instrument but maybe not things like the .plt or .fini/init
        // IF this changes, we need to change the code in the instrumentation code, save all the base addresses.

        if (SEC_IsExecutable(sec) && SEC_Name(sec) == ".text")
        {
            ADDRINT sec_addr = SEC_Address(sec);
            UINT64  sec_size = SEC_Size(sec);
            
            if (Knob_debug)
            {
                std::cout << "Name: " << SEC_Name(sec) << std::endl;
                std::cout << "Addr: 0x" << std::hex << sec_addr << std::endl;
                std::cout << "Size: " << sec_size << std::endl << std::endl;
            }

            if (sec_addr != 0)
            {
                ADDRINT high_addr = sec_addr + sec_size;

                if (sec_addr > min_addr || min_addr == 0)
                    min_addr = sec_addr;

                // Now check and set the max_addr.
                if (sec_addr > max_addr || max_addr == 0)
                    max_addr = sec_addr;

                if (high_addr > max_addr)
                    max_addr = high_addr;
            }
        }
    }
    if (Knob_debug)
    {
        std::cout << "min_addr:\t0x" << std::hex << min_addr << std::endl;
        std::cout << "max_addr:\t0x" << std::hex << max_addr << std::endl << std::endl;
    }   
}

// Main functions ------------------------------------------------

INT32 Usage()
{
    std::cerr << "AFLPIN -- A pin tool to enable blackbox binaries to be fuzzed with AFL on Linux/Windows" << std::endl;
    std::cerr << "   -debug --  prints extra debug information." << std::endl;
    std::cerr << "   -entry 0xADDR --  start address for coverage signal." << std::endl;
    std::cerr << "   -exit 0xADDR --  stop address for coverage signal." << std::endl;
    return -1;
}

#ifdef __win__
namespace windows {
    bool write_to_pipe(char *cmd)
    {
        DWORD bytes_writen;
        HANDLE afl_sync_handle;
        afl_sync_handle = CreateFile(
            "\\\\.\\pipe\\afl_sync",    // pipe name
            GENERIC_READ |              // read and write access
            GENERIC_WRITE,
            0,                          // no sharing
            NULL,                       // default security attributes
            OPEN_EXISTING,              // opens existing pipe
            0,                          // default attributes
            NULL);                                              // default security attribute

        if( afl_sync_handle == INVALID_HANDLE_VALUE )
            return false;

        WriteFile(afl_sync_handle, cmd, 1, &bytes_writen, 0);
        CloseHandle(afl_sync_handle);
        return true;
    }
}
#elif __linux__
bool write_to_pipe(char *cmd)
{
    //if( access("afl_sync", F_OK ) == -1 )
    //    mkfifo("afl_sync", 777);
    if(afl_sync_fd == -1)
        afl_sync_fd = open("afl_sync", O_WRONLY);
    write(afl_sync_fd, cmd, 1);
    return true;
}
#endif

#ifdef __win__
namespace windows {
    bool setup_shm()
    {
        HANDLE map_file;
        map_file = CreateFileMapping(
                    INVALID_HANDLE_VALUE,    // use paging file
                    NULL,                    // default security
                    PAGE_READWRITE,          // read/write access
                    0,                       // maximum object size (high-order DWORD)
                    MAP_SIZE,                // maximum object size (low-order DWORD)
                    (char *)"Local\\winapi-shm-1337");

        bitmap_shm = (unsigned char *) MapViewOfFile(map_file, // handle to map object
                FILE_MAP_ALL_ACCESS,  // read/write permission
                0,
                0,
                MAP_SIZE);
        memset(bitmap_shm, '\x00', MAP_SIZE);
        return true;
    }
}
#elif __linux__
bool setup_shm() {
    if (char *shm_key_str = getenv("__AFL_SHM_ID")) {
        int shm_id, shm_key;
        shm_key = atoi(shm_key_str);
        std::cout << "shm_key: " << shm_key << std::endl;        
	
        if( ( shm_id = shmget( (key_t)shm_key, MAP_SIZE, IPC_CREAT | IPC_EXCL | 0600 ) ) < 0 )  // try create by key
            shm_id = shmget( (key_t)shm_key, MAP_SIZE, IPC_EXCL | 0600 );  // find by key
        bitmap_shm = reinterpret_cast<uint8_t*>(shmat(shm_id, 0, 0));
        
        if (bitmap_shm == reinterpret_cast<void *>(-1)) {
            std::cout << "failed to get shm addr from shmmat()" << std::endl;
            return false;
        }
    }
    else {
        std::cout << "failed to get shm_id envvar" << std::endl;
        return false;
    }
    return true;
}
#endif

#ifdef __win__
void context_change(THREADID tid, CONTEXT_CHANGE_REASON reason, const CONTEXT *ctxtFrom, CONTEXT *ctxtTo, INT32 info, VOID *v)
{
    if(reason == CONTEXT_CHANGE_REASON_EXCEPTION)
    {
        printf("exception 0x%08x\n", info);
        if(info == 0xc0000005)
            windows::write_to_pipe("c");
    }
}
#elif __linux__
bool on_crash(unsigned int threadId, int sig, CONTEXT* ctx, bool hasHandler, const EXCEPTION_INFO* pExceptInfo, void* v)
{
  write_to_pipe( (char *) "c" );
  return true;
}
#endif


int main(int argc, char *argv[])
{
    if(PIN_Init(argc, argv)){
        return Usage();
    }

    #ifdef __win__
    windows::setup_shm();
    #elif __linux__
    setup_shm();
    #endif

    entry_addr = Knob_entry.Value();
    exit_addr = Knob_exit.Value();

    PIN_SetSyntaxIntel();
    TRACE_AddInstrumentFunction(Trace, 0);

    #ifdef __win__
    PIN_AddContextChangeFunction(context_change, 0);
    #elif __linux__
    PIN_InterceptSignal(SIGSEGV, on_crash, 0);
    #endif
    PIN_AddApplicationStartFunction(entry_point, 0);
    PIN_StartProgram();

    // AFL_NO_FORKSRV=1
    // We could use this main function to talk to the fork server's fd and then enable the fork server with this tool...
}

// https://github.com/carlosgprado/BrundleFuzz/blob/master/client_windows/MyPinTool.cpp