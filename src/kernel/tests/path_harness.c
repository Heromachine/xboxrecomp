#include <stdio.h>
#include <string.h>
#include <windows.h>
typedef int BOOLX;
extern void xbox_path_init(const char* game_dir, const char* save_dir);
extern BOOL xbox_translate_path(const char* xbox_path, WCHAR* buf, DWORD n);
/* The kernel library calls back into generated game code; stub the two
 * dispatch entry points so it links standalone. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va){ (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va){ (void)va; return 0; }

int main(void){
    WCHAR out[1024];
    const char* cases[] = {
        "\\Device\\CdRom0",
        "\\Device\\CdRom0\\romdata\\x.pak",
        "\\Device\\Harddisk0\\partition0",
        "\\Device\\Harddisk0\\Partition5",
        "\\Device\\Harddisk0\\Partition5\\foo.txt",
        "D:\\romdata\\x.pak",
        "T:\\save.dat",
    };
    xbox_path_init("GAMEDIR", "SAVEDIR");
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
        out[0]=0;
        BOOL ok = xbox_translate_path(cases[i], out, 1024);
        printf("CASE|%s|%d|%S\n", cases[i], (int)ok, out);
    }
    return 0;
}
