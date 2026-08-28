#include <stdint.h>

// ---------- 全局变量 ----------
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

// ---------- 文件系统 ----------
#define MAX_FILES 32
#define MAX_FILENAME 32
#define MAX_FILE_SIZE 2048

struct file_entry {
    char name[MAX_FILENAME];
    char data[MAX_FILE_SIZE];
    int size;
    int used;
};

static struct file_entry files[MAX_FILES];

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

// ---------- 文件系统初始化 ----------
void init_filesystem() {
    for (int i = 0; i < MAX_FILES; i++) {
        files[i].used = 0;
        files[i].size = 0;
        files[i].name[0] = '\0';
        files[i].data[0] = '\0';
    }
}

// 处理命令
static void process_command() {
    // -------- ls --------
    if (strcmp(cmd_buffer, "ls") == 1) {
        int count = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            if (files[i].used) {
                print(files[i].name);
                print("  ");
                char buf[16];
                print(itoa(files[i].size, buf));
                print(" bytes\n");
                count++;
            }
        }
        if (count == 0) print("No files.\n");
    }
    // -------- touch --------
    else if (starts_with(cmd_buffer, "touch ")) {
        const char* fname = cmd_buffer + 6;
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            print("Error: missing filename\n");
            goto done;
        }
        int found = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            if (files[i].used && strcmp(files[i].name, fname) == 1) {
                print("File already exists.\n");
                found = 1;
                break;
            }
        }
        if (!found) {
            for (int i = 0; i < MAX_FILES; i++) {
                if (!files[i].used) {
                    int j = 0;
                    while (fname[j] && j < MAX_FILENAME-1) {
                        files[i].name[j] = fname[j];
                        j++;
                    }
                    files[i].name[j] = '\0';
                    files[i].size = 0;
                    files[i].used = 1;
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
        int found = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            if (files[i].used && strcmp(files[i].name, fname) == 1) {
                if (files[i].size > 0) {
                    print(files[i].data);
                    print("\n");
                }
                found = 1;
                break;
            }
        }
        if (!found) print("File not found.\n");
        
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
        int found = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            if (files[i].used && strcmp(files[i].name, fname) == 1) {
                int j = 0;
                while (*rest && j < MAX_FILE_SIZE-1) {
                    files[i].data[j++] = *rest++;
                }
                files[i].data[j] = '\0';
                files[i].size = j;
                char buf[16];
                print("Written ");
                print(itoa(files[i].size, buf));
                print(" bytes.\n");
                found = 1;
                break;
            }
        }
        if (!found) print("File not found.\n");
    }
    // -------- rm --------
    else if (starts_with(cmd_buffer, "rm ")) {
        const char* fname = cmd_buffer + 3;
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            print("Error: missing filename\n");
            goto done;
        }
        int found = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            if (files[i].used && strcmp(files[i].name, fname) == 1) {
                files[i].used = 0;
                files[i].size = 0;
                files[i].name[0] = '\0';
                files[i].data[0] = '\0';
                print("File deleted.\n");
                found = 1;
                break;
            }
        }
        if (!found) print("File not found.\n");
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
        print("ls - List files\n");
        print("touch <name> - Create empty file\n");
        print("cat <name> - Show file content\n");
        print("write <name> <content> - Write content to file\n");
        print("rm <name> - Delete file\n");
    }else if (strcmp(cmd_buffer, "reboot") == 1) {
        print("Rebooting...\n");
        __asm__ volatile("int $0x19");
    }else if (strcmp(cmd_buffer, "version") == 1) {
        print("SunOS Kernel v0.3 (RamFS)\n");
    }else if (starts_with(cmd_buffer, "echo ")) {
        // 输出 echo 后面的内容（跳过 "echo " 这 5 个字符）
        const char* args = cmd_buffer + 5;
        print(args);
        print("\n");
    }else if (strcmp(cmd_buffer, "beep") == 1) {
        // 使用 PIT 通道 2 产生短促的“嘀”声
        __asm__ volatile(
            "mov $0xB6, %%al\n"
            "out %%al, $0x43\n"
            "mov $0x57, %%al\n"   // 频率值（低字节）
            "out %%al, $0x42\n"
            "mov $0x04, %%al\n"   // 频率值（高字节）
            "out %%al, $0x42\n"
            "in $0x61, %%al\n"
            "or $0x03, %%al\n"
            "out %%al, $0x61\n"
            "mov $0x10, %%cx\n"   // 延时循环
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
        // 关闭中断并停机
        __asm__ volatile("cli\n hlt");
        // 如果 hlt 被唤醒，再循环停机
        while (1);
    }else if (starts_with(cmd_buffer, "calc ")) {
        const char* expr = cmd_buffer + 5;  // 跳过 "calc "
        // 跳过前导空格
        while (*expr == ' ') expr++;
        
        // 解析第一个数字
        char num1_str[16];
        int i = 0;
        while (*expr != ' ' && *expr != '\0') {
            num1_str[i++] = *expr++;
        }
        num1_str[i] = '\0';
        int num1 = atoi(num1_str);
        
        // 跳过空格，找运算符
        while (*expr == ' ') expr++;
        if (*expr == '\0') {
            print("Error: missing operator\n");
            goto calc_done;
        }
        char op = *expr++;
        
        // 跳过空格，找第二个数字
        while (*expr == ' ') expr++;
        i = 0;
        char num2_str[16];
        while (*expr != '\0' && *expr != ' ') {
            num2_str[i++] = *expr++;
        }
        num2_str[i] = '\0';
        int num2 = atoi(num2_str);
        
        // 执行计算
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
        // 无需额外操作，之后重置缓冲区
    }else if (cmd_buffer[0] != 0) {
        print("Unknown command: ");
        print(cmd_buffer);
        print("\n");
    }
    done:
    // 无论命令是否有效，重置缓冲区并显示新提示符
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
            // 数字行符号
            case 0x02: c = '!'; break;   // 1
            case 0x03: c = '@'; break;   // 2
            case 0x04: c = '#'; break;   // 3
            case 0x05: c = '$'; break;   // 4
            case 0x06: c = '%'; break;   // 5
            case 0x07: c = '^'; break;   // 6
            case 0x08: c = '&'; break;   // 7
            case 0x09: c = '*'; break;   // 8  ← 这就是乘号！
            case 0x0A: c = '('; break;   // 9
            case 0x0B: c = ')'; break;   // 0
            case 0x0C: c = '_'; break;   // -
            case 0x0D: c = '+'; break;   // =  ← 加号！
            // 字母大写
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
            case 0x1A: c = '{'; break;   // [
            case 0x1B: c = '}'; break;   // ]
            case 0x1E: c = 'A'; break;
            case 0x1F: c = 'S'; break;
            case 0x20: c = 'D'; break;
            case 0x21: c = 'F'; break;
            case 0x22: c = 'G'; break;
            case 0x23: c = 'H'; break;
            case 0x24: c = 'J'; break;
            case 0x25: c = 'K'; break;
            case 0x26: c = 'L'; break;
            case 0x27: c = ':'; break;   // ;
            case 0x28: c = '"'; break;   // '
            case 0x29: c = '~'; break;   // `
            case 0x2B: c = '|'; break;   // \
            case 0x2C: c = 'Z'; break;
            case 0x2D: c = 'X'; break;
            case 0x2E: c = 'C'; break;
            case 0x2F: c = 'V'; break;
            case 0x30: c = 'B'; break;
            case 0x31: c = 'N'; break;
            case 0x32: c = 'M'; break;
            case 0x33: c = '<'; break;   // ,
            case 0x34: c = '>'; break;   // .
            case 0x35: c = '?'; break;   // /
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
    if (scancode == 0x2A || scancode == 0x36) {   // 左或右 Shift 按下
        shift_pressed = 1;
        __asm__ volatile("mov $0x20, %%al\n out %%al, $0x20" : : : "eax");
        disable_hardware_cursor();
        return;   // 不处理其他逻辑
    } else if (scancode == 0xAA || scancode == 0xB6) { // 左或右 Shift 释放
        shift_pressed = 0;
        __asm__ volatile("mov $0x20, %%al\n out %%al, $0x20" : : : "eax");
        disable_hardware_cursor();
        return;
    }
    if (scancode == 0x0E) {  // 退格
        if (cmd_index > 0) {
            cmd_index--;
            backspace();
        }
    } else {
        char ascii = scancode_to_ascii(scancode);
        if (ascii != 0) {
            if (ascii == '\n') {
                // 处理命令
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
    disable_hardware_cursor();  // 额外保险
    print("  _   _                     ___   _____ \n");
    print(" | \\ | |                   / _ \\ / ____|\n");
    print(" |  \\| | _____   _____ _ _| | | | (___  \n");
    print(" | . ` |/ _ \\ \\ / / _ \\ '__| | | |\\___ \\ \n");
    print(" | |\\  | (_) \\ V /  __/ |  | |_| |____) |\n");
    print(" |_| \\_|\\___/ \\_/ \\___|_|   \\___/|_____/ \n");
    print("\n");
    print("            Nova OS     Loading...   \n");

    // 初始化文件系统
    init_filesystem();
    print("Nova OS Kernel v0.3 (RamFS)\n");
    print("Type a command and press Enter.\n");
    print("> ");   // 命令提示符

    __asm__ volatile("sti");

    while (1) {
        // 空闲
    }
}