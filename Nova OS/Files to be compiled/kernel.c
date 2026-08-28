#include <stdint.h>

//全局变量
volatile int cursor_x = 0;
volatile int cursor_y = 0;
volatile int interrupt_counter = 0;
volatile int shift_pressed = 0;   // 用于记录 Shift 是否被按下
// 软件光标状态
static int old_cursor_x = 0;
static int old_cursor_y = 0;
static char saved_char = ' ';
static uint8_t saved_attr = 0x07;
static int cursor_visible = 1;

// 命令缓冲区
#define CMD_BUFFER_SIZE 256
static char cmd_buffer[CMD_BUFFER_SIZE];
static int cmd_index = 0;

//文件系统
#define MAX_FILES 64
#define MAX_FILENAME 32
#define MAX_FILE_SIZE 2048
#define MAX_PATH 128

struct file_entry {
    char name[MAX_FILENAME];
    char data[MAX_FILE_SIZE];
    int size;
    int used;
    int isdir;          // 1 表示目录，0 表示文件
    int parent;         // 父目录索引（-1 表示根目录）
};

static struct file_entry files[MAX_FILES];
static int current_dir = 0;  // 当前工作目录索引（0 为根目录）

// 前置声明
void putchar(char c);
void clear_screen();
void update_cursor();
void scroll();
void backspace();
void print(const char* str);
void disable_hardware_cursor();

//程序光标有点Bug，所以要禁用硬件光标
// 禁用硬件光标
void disable_hardware_cursor() {
    __asm__ volatile(
        "mov $0x0A, %%al\n"
        "out %%al, $0x3D4\n"
        "mov $0x1F, %%al\n"
        "out %%al, $0x3D5\n"
        "mov $0x0B, %%al\n"
        "out %%al, $0x3D4\n"
        "mov $0x00, %%al\n"
        "out %%al, $0x3D5\n"
        : : : "al", "dx", "memory"
    );
    __asm__ volatile(
        "mov $0x0E, %%al\n"
        "out %%al, $0x3D4\n"
        "mov $0x07, %%al\n"
        "out %%al, $0x3D5\n"
        "mov $0x0F, %%al\n"
        "out %%al, $0x3D4\n"
        "mov $0xD0, %%al\n"
        "out %%al, $0x3D5\n"
        : : : "al", "dx", "memory"
    );
}

//软件光标控制 
void erase_cursor() {
    if (cursor_visible) {
        char* video = (char*)0xB8000;
        int pos = old_cursor_y * 80 + old_cursor_x;
        video[2*pos] = saved_char;
        video[2*pos+1] = saved_attr;
        cursor_visible = 0;
    }
}

void draw_cursor() {
    char* video = (char*)0xB8000;
    int pos = cursor_y * 80 + cursor_x;
    saved_char = video[2*pos];
    saved_attr = video[2*pos+1];
    video[2*pos] = '_';
    video[2*pos+1] = 0x0F;
    cursor_visible = 1;
    old_cursor_x = cursor_x;
    old_cursor_y = cursor_y;
}

void update_cursor() {
    if (old_cursor_x != cursor_x || old_cursor_y != cursor_y) {
        erase_cursor();
    }
    draw_cursor();
    disable_hardware_cursor();  // 保险
}

// 清屏
void clear_screen() {
    char* video = (char*)0xB8000;
    for (int i = 0; i < 80 * 25; i++) {
        video[2*i] = ' ';
        video[2*i+1] = 0x07;
    }
    cursor_x = 0;
    cursor_y = 0;
    old_cursor_x = 0;
    old_cursor_y = 0;
    saved_char = ' ';
    saved_attr = 0x07;
    cursor_visible = 0;
    disable_hardware_cursor();
    update_cursor();
}

// 滚动
void scroll() {
    erase_cursor();
    char* video = (char*)0xB8000;
    for (int row = 1; row < 25; row++) {
        for (int col = 0; col < 80; col++) {
            int src = row * 80 + col;
            int dst = (row - 1) * 80 + col;
            video[2*dst] = video[2*src];
            video[2*dst+1] = video[2*src+1];
        }
    }
    for (int col = 0; col < 80; col++) {
        int pos = 24 * 80 + col;
        video[2*pos] = ' ';
        video[2*pos+1] = 0x07;
    }
    cursor_y = 24;
    cursor_x = 0;
    update_cursor();
}

//输出字符
void putchar(char c) {
    erase_cursor();
    char* video = (char*)0xB8000;
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else {
        int pos = cursor_y * 80 + cursor_x;
        video[2*pos] = c;
        video[2*pos+1] = 0x07;
        cursor_x++;
        if (cursor_x >= 80) {
            cursor_x = 0;
            cursor_y++;
        }
    }
    if (cursor_y >= 25) {
        scroll();
    } else {
        update_cursor();
    }
}

//  打印字符串（仅支持 \n）
void print(const char* str) {
    while (*str) {
        if (*str == '\n') {
            putchar('\n');
        } else {
            putchar(*str);
        }
        str++;
    }
}

// 退格
void backspace() {
    erase_cursor();
    if (cursor_x > 0) {
        cursor_x--;
    } else if (cursor_y > 0) {
        cursor_y--;
        cursor_x = 79;
    } else {
        update_cursor();
        return;
    }
    char* video = (char*)0xB8000;
    int pos = cursor_y * 80 + cursor_x;
    video[2*pos] = ' ';
    video[2*pos+1] = 0x07;
    update_cursor();
}

//字符串比较 
int strcmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == *b);
}
// 检查字符串是否以指定前缀开头
int starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++;
        prefix++;
    }
    return 1;  // 前缀匹配
}
// 工具函数
// 将字符串转为整数（忽略前导空格，支持正负号）
int atoi(const char* str) {
    int result = 0;
    int sign = 1;
    while (*str == ' ') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result * sign;
}

// 将整数转为字符串（存入缓冲区，返回缓冲区指针）
char* itoa(int num, char* buffer) {
    char* ptr = buffer;
    int is_negative = 0;
    if (num < 0) {
        is_negative = 1;
        num = -num;
    }
    // 先倒序写入
    char temp[16];
    int i = 0;
    if (num == 0) temp[i++] = '0';
    while (num > 0) {
        temp[i++] = '0' + (num % 10);
        num /= 10;
    }
    if (is_negative) temp[i++] = '-';
    // 反转
    while (i > 0) {
        *ptr++ = temp[--i];
    }
    *ptr = '\0';
    return buffer;
}

//路径工具函数
// 复制字符串到目标（安全版）
void strcpy_safe(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

// 判断是否为目录
int is_directory(int idx) {
    return files[idx].used && files[idx].isdir;
}

// 查找子目录/文件（在当前目录中查找）
int find_entry(int parent_idx, const char* name) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].used && files[i].parent == parent_idx && strcmp(files[i].name, name) == 1) {
            return i;
        }
    }
    return -1;
}

// 解析路径，返回目标索引，同时返回父目录索引
int resolve_path(const char* path, int* parent_out) {
    if (path[0] == '\0') {
        if (parent_out) *parent_out = current_dir;
        return current_dir;
    }
    
    int current = current_dir;
    char component[MAX_FILENAME];
    const char* rest = path;
    
    // 绝对路径：从根目录开始
    if (path[0] == '/') {
        current = 0;  // 根目录索引为 0
        rest = path + 1;
    }
    
    // 逐级解析
    while (*rest) {
        // 提取第一个组件
        int i = 0;
        while (rest[i] && rest[i] != '/') {
            if (i < MAX_FILENAME - 1) component[i] = rest[i];
            i++;
        }
        component[i] = '\0';
        
        // 跳过 / 检查剩余部分
        while (rest[i] == '/') i++;
        rest = rest + i;
        
        // 如果组件为空（路径结尾），返回当前目录
        if (component[0] == '\0') {
            if (parent_out) *parent_out = current;
            return current;
        }
        
        // 处理 ".."
        if (strcmp(component, "..") == 1) {
            if (files[current].parent != -1) {
                current = files[current].parent;
            }
            if (*rest == '\0') {
                if (parent_out) *parent_out = current;
                return current;
            }
            continue;
        }
        
        // 处理 "."
        if (strcmp(component, ".") == 1) {
            if (*rest == '\0') {
                if (parent_out) *parent_out = current;
                return current;
            }
            continue;
        }
        
        // 在当前目录中查找
        int found = -1;
        for (int j = 0; j < MAX_FILES; j++) {
            if (files[j].used && files[j].parent == current && strcmp(files[j].name, component) == 1) {
                found = j;
                break;
            }
        }
        
        if (found == -1) {
            if (parent_out) *parent_out = -1;
            return -1;
        }
        
        if (!files[found].isdir && *rest != '\0') {
            if (parent_out) *parent_out = -1;
            return -1;
        }
        
        current = found;
        if (*rest == '\0') {
            if (parent_out) *parent_out = current;
            return current;
        }
    }
    
    if (parent_out) *parent_out = current;
    return current;
}

// 获取当前路径（存入 buffer）
void get_current_path(char* buffer, int max_len) {
    if (current_dir == 0) {
        strcpy_safe(buffer, "/", max_len);
        return;
    }
    
    int path_stack[MAX_FILES];
    int depth = 0;
    int idx = current_dir;
    while (idx != 0 && idx != -1) {
        path_stack[depth++] = idx;
        idx = files[idx].parent;
    }
    
    int pos = 0;
    if (depth == 0) {
        buffer[pos++] = '/';
    } else {
        for (int i = depth - 1; i >= 0; i--) {
            buffer[pos++] = '/';
            int j = 0;
            while (files[path_stack[i]].name[j] && pos < max_len - 1) {
                buffer[pos++] = files[path_stack[i]].name[j++];
            }
        }
    }
    buffer[pos] = '\0';
}

//文件系统初始化
void init_filesystem() {
    for (int i = 0; i < MAX_FILES; i++) {
        files[i].used = 0;
        files[i].size = 0;
        files[i].isdir = 0;
        files[i].parent = -1;
        files[i].name[0] = '\0';
        files[i].data[0] = '\0';
    }
    // 创建根目录（索引 0）
    files[0].used = 1;
    files[0].isdir = 1;
    files[0].parent = -1;
    files[0].size = 0;
    strcpy_safe(files[0].name, "/", MAX_FILENAME);
    current_dir = 0;
}

// ---------- 获取文件名（从路径中提取最后一个组件） ----------
const char* get_filename_from_path(const char* path) {
    const char* p = path;
    const char* last = path;
    while (*p) {
        if (*p == '/') last = p + 1;
        p++;
    }
    return last;
}

// ---------- 处理命令 ----------
static void process_command() {
    // -------- ls --------
    if (strcmp(cmd_buffer, "ls") == 1) {
        int count = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            if (files[i].used && files[i].parent == current_dir) {
                if (files[i].isdir) {
                    print("[DIR]  ");
                } else {
                    print("[FILE] ");
                }
                print(files[i].name);
                print("  ");
                char buf[16];
                print(itoa(files[i].size, buf));
                print(" bytes\n");
                count++;
            }
        }
        if (count == 0) print("(empty)\n");
    }
    // -------- mkdir --------
    else if (starts_with(cmd_buffer, "mkdir ")) {
        const char* dname = cmd_buffer + 6;
        while (*dname == ' ') dname++;
        if (*dname == '\0') {
            print("Error: missing directory name\n");
            goto done;
        }
        // 检查是否包含路径分隔符
        int has_slash = 0;
        const char* p = dname;
        while (*p) { if (*p == '/') { has_slash = 1; break; } p++; }
        
        if (has_slash) {
            // 含路径：解析父目录
            int parent;
            int target = resolve_path(dname, &parent);
            if (target != -1) {
                print("Error: already exists\n");
                goto done;
            }
            if (parent == -1) {
                print("Error: parent directory not found\n");
                goto done;
            }
            const char* name = get_filename_from_path(dname);
            if (*name == '\0') {
                print("Error: invalid name\n");
                goto done;
            }
            // 创建目录，父目录为 parent
            for (int i = 0; i < MAX_FILES; i++) {
                if (!files[i].used) {
                    strcpy_safe(files[i].name, name, MAX_FILENAME);
                    files[i].used = 1;
                    files[i].isdir = 1;
                    files[i].parent = parent;
                    files[i].size = 0;
                    files[i].data[0] = '\0';
                    print("Directory created.\n");
                    break;
                }
            }
        } else {
            // 简单名称，父目录为当前目录
            int found = find_entry(current_dir, dname);
            if (found != -1) {
                print("Error: already exists\n");
                goto done;
            }
            for (int i = 0; i < MAX_FILES; i++) {
                if (!files[i].used) {
                    strcpy_safe(files[i].name, dname, MAX_FILENAME);
                    files[i].used = 1;
                    files[i].isdir = 1;
                    files[i].parent = current_dir;
                    files[i].size = 0;
                    files[i].data[0] = '\0';
                    print("Directory created.\n");
                    break;
                }
            }
        }
    }
    // -------- cd --------
    else if (starts_with(cmd_buffer, "cd ")) {
        const char* path = cmd_buffer + 3;
        while (*path == ' ') path++;
        if (*path == '\0') {
            current_dir = 0;
            char buf[MAX_PATH];
            get_current_path(buf, MAX_PATH);
            print("Changed to: ");
            print(buf);
            print("\n");
            goto done;
        }
        int parent;
        int target = resolve_path(path, &parent);
        if (target == -1 || !files[target].isdir) {
            print("Error: directory not found\n");
        } else {
            current_dir = target;
            char buf[MAX_PATH];
            get_current_path(buf, MAX_PATH);
            print("Changed to: ");
            print(buf);
            print("\n");
        }
    }
    // -------- pwd --------
    else if (strcmp(cmd_buffer, "pwd") == 1) {
        char buf[MAX_PATH];
        get_current_path(buf, MAX_PATH);
        print(buf);
        print("\n");
    }
    // -------- touch --------
    else if (starts_with(cmd_buffer, "touch ")) {
        const char* fname = cmd_buffer + 6;
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            print("Error: missing filename\n");
            goto done;
        }
        // 检查是否包含路径分隔符
        int has_slash = 0;
        const char* p = fname;
        while (*p) { if (*p == '/') { has_slash = 1; break; } p++; }
        
        if (has_slash) {
            // 含路径：解析父目录
            int parent;
            int target = resolve_path(fname, &parent);
            if (target != -1) {
                print("Error: already exists\n");
                goto done;
            }
            if (parent == -1) {
                print("Error: parent directory not found\n");
                goto done;
            }
            const char* name = get_filename_from_path(fname);
            if (*name == '\0') {
                print("Error: invalid name\n");
                goto done;
            }
            // 创建文件，父目录为 parent
            for (int i = 0; i < MAX_FILES; i++) {
                if (!files[i].used) {
                    strcpy_safe(files[i].name, name, MAX_FILENAME);
                    files[i].used = 1;
                    files[i].isdir = 0;
                    files[i].parent = parent;
                    files[i].size = 0;
                    files[i].data[0] = '\0';
                    print("File created.\n");
                    break;
                }
            }
        } else {
            // 简单名称，父目录为当前目录
            int found = find_entry(current_dir, fname);
            if (found != -1) {
                print("Error: already exists\n");
                goto done;
            }
            for (int i = 0; i < MAX_FILES; i++) {
                if (!files[i].used) {
                    strcpy_safe(files[i].name, fname, MAX_FILENAME);
                    files[i].used = 1;
                    files[i].isdir = 0;
                    files[i].parent = current_dir;
                    files[i].size = 0;
                    files[i].data[0] = '\0';
                    print("File created.\n");
                    break;
                }
            }
        }
    }
    // -------- cat --------
    else if (starts_with(cmd_buffer, "cat ")) {
        const char* fname = cmd_buffer + 4;
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            print("Error: missing filename\n");
            goto done;
        }
        int parent;
        int target = resolve_path(fname, &parent);
        if (target == -1 || files[target].isdir) {
            print("File not found.\n");
            goto done;
        }
        if (files[target].size > 0) {
            print(files[target].data);
            print("\n");
        }
    }
    // -------- write --------
    else if (starts_with(cmd_buffer, "write ")) {
        const char* rest = cmd_buffer + 6;
        while (*rest == ' ') rest++;
        char fname[MAX_FILENAME];
        int i = 0;
        while (*rest != ' ' && *rest != '\0') {
            if (i < MAX_FILENAME-1) fname[i++] = *rest;
            rest++;
        }
        fname[i] = '\0';
        while (*rest == ' ') rest++;
        if (*rest == '\0') {
            print("Error: no content to write\n");
            goto done;
        }
        int parent;
        int target = resolve_path(fname, &parent);
        if (target == -1 || files[target].isdir) {
            print("File not found.\n");
            goto done;
        }
        int j = 0;
        while (*rest && j < MAX_FILE_SIZE-1) {
            files[target].data[j++] = *rest++;
        }
        files[target].data[j] = '\0';
        files[target].size = j;
        char buf[16];
        print("Written ");
        print(itoa(files[target].size, buf));
        print(" bytes.\n");
    }
    // -------- rm --------
    else if (starts_with(cmd_buffer, "rm ")) {
        const char* fname = cmd_buffer + 3;
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            print("Error: missing filename\n");
            goto done;
        }
        int parent;
        int target = resolve_path(fname, &parent);
        if (target == -1) {
            print("File not found.\n");
            goto done;
        }
        if (files[target].isdir) {
            // 检查目录是否为空
            int empty = 1;
            for (int i = 0; i < MAX_FILES; i++) {
                if (files[i].used && files[i].parent == target) {
                    empty = 0;
                    break;
                }
            }
            if (!empty) {
                print("Error: directory not empty\n");
                goto done;
            }
        }
        files[target].used = 0;
        files[target].size = 0;
        files[target].name[0] = '\0';
        files[target].data[0] = '\0';
        print("Deleted.\n");
    }
    // -------- cls --------
    else if (strcmp(cmd_buffer, "cls") == 1) {
        clear_screen();
        print("Screen cleared.\n");
    } else if (strcmp(cmd_buffer, "help") == 1){
        print("cls - Clear screen\n");
        print("help - Show this help message\n");
        print("reboot - Reboot the system\n");
        print("version - Show version info\n");
        print("echo - Print arguments\n");
        print("beep - Make a beep sound\n");
        print("shutdown - Shutdown the system\n");
        print("calc - Perform simple calculations\n");
        print("ls - List files in current directory\n");
        print("mkdir <name> - Create directory\n");
        print("cd <path> - Change directory (.. for parent)\n");
        print("pwd - Print working directory\n");
        print("touch <name> - Create empty file\n");
        print("cat <name> - Show file content\n");
        print("write <name> <content> - Write content to file\n");
        print("rm <name> - Delete file or empty directory\n");
    }else if (strcmp(cmd_buffer, "reboot") == 1) {
        print("Rebooting...\n");
        __asm__ volatile("int $0x19");
    }else if (strcmp(cmd_buffer, "version") == 1) {
        print("Nova OS Kernel v0.4 \n");
    }else if (starts_with(cmd_buffer, "echo ")) {
        const char* args = cmd_buffer + 5;
        print(args);
        print("\n");
    }else if (strcmp(cmd_buffer, "beep") == 1) {
        __asm__ volatile(
            "mov $0xB6, %%al\n"
            "out %%al, $0x43\n"
            "mov $0x57, %%al\n"
            "out %%al, $0x42\n"
            "mov $0x04, %%al\n"
            "out %%al, $0x42\n"
            "in $0x61, %%al\n"
            "or $0x03, %%al\n"
            "out %%al, $0x61\n"
            "mov $0x10, %%cx\n"
            ".loop:\n"
            "loop .loop\n"
            "in $0x61, %%al\n"
            "and $0xFC, %%al\n"
            "out %%al, $0x61\n"
            : : : "eax", "ecx", "dx"
        );
        print("Beep OK\n");
    }else if (strcmp(cmd_buffer, "shutdown") == 1) {
        print("System halted.\n");
        __asm__ volatile("cli\n hlt");
        while (1);
    }else if (starts_with(cmd_buffer, "calc ")) {
        const char* expr = cmd_buffer + 5;
        while (*expr == ' ') expr++;
        char num1_str[16];
        int i = 0;
        while (*expr != ' ' && *expr != '\0') {
            num1_str[i++] = *expr++;
        }
        num1_str[i] = '\0';
        int num1 = atoi(num1_str);
        while (*expr == ' ') expr++;
        if (*expr == '\0') {
            print("Error: missing operator\n");
            goto calc_done;
        }
        char op = *expr++;
        while (*expr == ' ') expr++;
        i = 0;
        char num2_str[16];
        while (*expr != '\0' && *expr != ' ') {
            num2_str[i++] = *expr++;
        }
        num2_str[i] = '\0';
        int num2 = atoi(num2_str);
        int result;
        int valid = 1;
        switch (op) {
            case '+': result = num1 + num2; break;
            case '-': result = num1 - num2; break;
            case '*': result = num1 * num2; break;
            case '/':
                if (num2 == 0) {
                    print("Error: division by zero\n");
                    valid = 0;
                } else {
                    result = num1 / num2;
                }
                break;
            default:
                print("Error: invalid operator\n");
                valid = 0;
                break;
        }
        if (valid) {
            char buf[32];
            print(itoa(result, buf));
            print("\n");
        }
    calc_done:
        ;
    }else if (cmd_buffer[0] != 0) {
        print("Unknown command: ");
        print(cmd_buffer);
        print("\n");
    }
    done:
    cmd_index = 0;
    cmd_buffer[0] = '\0';
    print("> ");
}

//后边的注释是这样的↓
// ---------- 扫描码映射 ----------
char scancode_to_ascii(unsigned char scancode) {
    // 基础映射（无 Shift）
    static const char base_map[] = {
        0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
        'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
        'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    if (scancode > 0x58) return 0;
    char c = base_map[scancode];

    // 如果 Shift 被按下，转换为大写或符号
    if (shift_pressed) {
        switch (scancode) {
            case 0x02: c = '!'; break;
            case 0x03: c = '@'; break;
            case 0x04: c = '#'; break;
            case 0x05: c = '$'; break;
            case 0x06: c = '%'; break;
            case 0x07: c = '^'; break;
            case 0x08: c = '&'; break;
            case 0x09: c = '*'; break;
            case 0x0A: c = '('; break;
            case 0x0B: c = ')'; break;
            case 0x0C: c = '_'; break;
            case 0x0D: c = '+'; break;
            case 0x10: c = 'Q'; break;
            case 0x11: c = 'W'; break;
            case 0x12: c = 'E'; break;
            case 0x13: c = 'R'; break;
            case 0x14: c = 'T'; break;
            case 0x15: c = 'Y'; break;
            case 0x16: c = 'U'; break;
            case 0x17: c = 'I'; break;
            case 0x18: c = 'O'; break;
            case 0x19: c = 'P'; break;
            case 0x1A: c = '{'; break;
            case 0x1B: c = '}'; break;
            case 0x1E: c = 'A'; break;
            case 0x1F: c = 'S'; break;
            case 0x20: c = 'D'; break;
            case 0x21: c = 'F'; break;
            case 0x22: c = 'G'; break;
            case 0x23: c = 'H'; break;
            case 0x24: c = 'J'; break;
            case 0x25: c = 'K'; break;
            case 0x26: c = 'L'; break;
            case 0x27: c = ':'; break;
            case 0x28: c = '"'; break;
            case 0x29: c = '~'; break;
            case 0x2B: c = '|'; break;
            case 0x2C: c = 'Z'; break;
            case 0x2D: c = 'X'; break;
            case 0x2E: c = 'C'; break;
            case 0x2F: c = 'V'; break;
            case 0x30: c = 'B'; break;
            case 0x31: c = 'N'; break;
            case 0x32: c = 'M'; break;
            case 0x33: c = '<'; break;
            case 0x34: c = '>'; break;
            case 0x35: c = '?'; break;
            default: break;
        }
    }
    return c;
}

// ---------- GDT ----------
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct gdt_entry gdt[3];
struct gdt_ptr gp;

void setup_gdt() {
    gdt[0] = (struct gdt_entry){0,0,0,0,0,0};
    gdt[1] = (struct gdt_entry){0xFFFF,0,0,0x9A,0xCF,0};
    gdt[2] = (struct gdt_entry){0xFFFF,0,0,0x92,0xCF,0};
    gp.limit = sizeof(gdt) - 1;
    gp.base = (uint32_t)&gdt;
    __asm__ volatile("lgdt (%0)" : : "r" (&gp));
    __asm__ volatile(
        "ljmp $0x08, $1f\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        : : : "eax"
    );
}

// ---------- IDT ----------
struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr ip;

extern void keyboard_handler_wrapper();

// 键盘中断处理
void keyboard_handler_c() {
    interrupt_counter++;
    unsigned char scancode;
    __asm__ volatile("in $0x60, %0" : "=a"(scancode));
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        __asm__ volatile("mov $0x20, %%al\n out %%al, $0x20" : : : "eax");
        disable_hardware_cursor();
        return;
    } else if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = 0;
        __asm__ volatile("mov $0x20, %%al\n out %%al, $0x20" : : : "eax");
        disable_hardware_cursor();
        return;
    }
    if (scancode == 0x0E) {
        if (cmd_index > 0) {
            cmd_index--;
            backspace();
        }
    } else {
        char ascii = scancode_to_ascii(scancode);
        if (ascii != 0) {
            if (ascii == '\n') {
                cmd_buffer[cmd_index] = '\0';
                putchar('\n');
                process_command();
            } else {
                if (cmd_index < CMD_BUFFER_SIZE - 1) {
                    cmd_buffer[cmd_index++] = ascii;
                    cmd_buffer[cmd_index] = '\0';
                }
                putchar(ascii);
            }
        }
    }
    __asm__ volatile("mov $0x20, %%al\n out %%al, $0x20" : : : "eax");
    disable_hardware_cursor();
}

void setup_idt() {
    ip.limit = sizeof(idt) - 1;
    ip.base = (uint32_t)&idt;
    for (int i = 0; i < 256; i++) {
        idt[i].base_low = 0;
        idt[i].sel = 0x08;
        idt[i].always0 = 0;
        idt[i].flags = 0x8E;
        idt[i].base_high = 0;
    }
    uint32_t handler_addr = (uint32_t)keyboard_handler_wrapper;
    idt[0x21].base_low = handler_addr & 0xFFFF;
    idt[0x21].sel = 0x08;
    idt[0x21].always0 = 0;
    idt[0x21].flags = 0x8E;
    idt[0x21].base_high = (handler_addr >> 16) & 0xFFFF;
    __asm__ volatile("lidt (%0)" : : "r" (&ip));
}

// ---------- PIC ----------
void setup_pic() {
    __asm__ volatile(
        "mov $0x11, %%al\n"
        "out %%al, $0x20\n"
        "out %%al, $0xA0\n"
        "mov $0x20, %%al\n"
        "out %%al, $0x21\n"
        "mov $0x28, %%al\n"
        "out %%al, $0xA1\n"
        "mov $0x04, %%al\n"
        "out %%al, $0x21\n"
        "mov $0x02, %%al\n"
        "out %%al, $0xA1\n"
        "mov $0x01, %%al\n"
        "out %%al, $0x21\n"
        "out %%al, $0xA1\n"
        : : : "eax"
    );
    __asm__ volatile(
        "mov $0xFD, %%al\n"
        "out %%al, $0x21\n"
        "mov $0xFF, %%al\n"
        "out %%al, $0xA1\n"
        : : : "eax"
    );
}

// ---------- 内核入口 ----------
__attribute__((noreturn))
void kmain(unsigned int magic, unsigned int addr) {
    setup_gdt();
    setup_idt();
    setup_pic();

    clear_screen();
    disable_hardware_cursor();

    print("  _   _                     ___   _____ \n");
    print(" | \\ | |                   / _ \\ / ____|\n");
    print(" |  \\| | _____   _____ _ _| | | | (___  \n");
    print(" | . ` |/ _ \\ \\ / / _ \\ '__| | | |\\___ \\ \n");
    print(" | |\\  | (_) \\ V /  __/ |  | |_| |____) |\n");
    print(" |_| \\_|\\___/ \\_/ \\___|_|   \\___/|_____/ \n");
    print("\n");
    print("           Nova OS v0.4      \n");
    print("\n");

    init_filesystem();
    print("Nova OS Kernel v0.4 \n");
    print("Commands: ls, mkdir, cd, pwd, touch, cat, write, rm, cls, help, reboot, version, echo, beep(Beta), shutdown, calc\n");
    print("> ");

    __asm__ volatile("sti");

    while (1) {
        // 空闲
    }
}