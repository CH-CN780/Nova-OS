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

// 命令历史
#define HISTORY_SIZE 16
static char history[HISTORY_SIZE][CMD_BUFFER_SIZE];
static int history_count = 0;        // 已存储的命令数
static int history_pos = 0;          // 当前浏览位置（0 表示未浏览）
static int history_tail = 0;         // 下一个要写入的位置

//文件系统
#define MAX_FILES 64
#define MAX_FILENAME 32
#define MAX_FILE_SIZE 2048
#define MAX_PATH 128
// ---------- ATA PIO 驱动 ----------
#define ATA_PRIMARY_IO       0x1F0
#define ATA_PRIMARY_CTRL     0x3F6
#define ATA_SECTOR_SIZE      512
// ---------- FAT12 数据结构 ----------
#define FAT12_SECTOR_SIZE    512
#define FAT12_SECTORS_PER_CLUSTER 1
#define FAT12_RESERVED_SECTORS 1    // 引导扇区
#define FAT12_FAT_COUNT      2
#define FAT12_ROOT_ENTRIES   224
#define FAT12_MAX_FILENAME   11
// ---------- FAT12 全局变量 ----------
static uint8_t fat12_sector_cache[FAT12_SECTOR_SIZE];
static uint16_t fat12_sectors_per_fat;
static uint16_t fat12_root_dir_sector;
static uint16_t fat12_data_sector;
static int fat12_mounted = 0;
static uint16_t fat12_reserved_sectors = 1;
// FAT12 当前目录状态
static uint16_t current_fat_cluster = 0;  // 0 表示根目录
static char current_fat_path[128] = "/";
// FAT12 目录操作缓冲区（足够容纳根目录 14 个扇区）
static uint8_t fat12_dir_buffer[FAT12_SECTOR_SIZE * 14];
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
//禁用硬件光标
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
// 等待磁盘就绪
static void ata_wait_ready() {
    uint8_t status;
    do {
        __asm__ volatile(
            "mov $0x1F7, %%dx\n"
            "in %%dx, %%al"
            : "=a"(status)
            : : "dx"
        );
    } while (status & 0x80);
}
// 读取一个扇区
void ata_read_sector(uint32_t lba, uint8_t* buffer) {
    ata_wait_ready();

    uint8_t lba_low = lba & 0xFF;
    uint8_t lba_mid = (lba >> 8) & 0xFF;
    uint8_t lba_high = (lba >> 16) & 0xFF;

    // 选择 LBA 模式
    __asm__ volatile("mov $0x1F6, %%dx\n mov $0xE0, %%al\n out %%al, %%dx" : : : "al", "dx");
    // 扇区数 = 1
    __asm__ volatile("mov $0x1F2, %%dx\n mov $0x01, %%al\n out %%al, %%dx" : : : "al", "dx");
    // LBA 低 8 位
    __asm__ volatile("mov $0x1F3, %%dx\n mov %0, %%al\n out %%al, %%dx" : : "a" (lba_low) : "dx");
    // LBA 中 8 位
    __asm__ volatile("mov $0x1F4, %%dx\n mov %0, %%al\n out %%al, %%dx" : : "a" (lba_mid) : "dx");
    // LBA 高 8 位
    __asm__ volatile("mov $0x1F5, %%dx\n mov %0, %%al\n out %%al, %%dx" : : "a" (lba_high) : "dx");
    // 读命令
    __asm__ volatile("mov $0x1F7, %%dx\n mov $0x20, %%al\n out %%al, %%dx" : : : "al", "dx");

    ata_wait_ready();
    for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
        uint16_t word;
        __asm__ volatile(
            "mov $0x1F0, %%dx\n"
            "in %%dx, %%ax"
            : "=a"(word)
            : : "dx"
        );
        buffer[i*2] = word & 0xFF;
        buffer[i*2+1] = (word >> 8) & 0xFF;
    }
}

// 写入一个扇区
void ata_write_sector(uint32_t lba, const uint8_t* buffer) {
    ata_wait_ready();

    uint8_t lba_low = lba & 0xFF;
    uint8_t lba_mid = (lba >> 8) & 0xFF;
    uint8_t lba_high = (lba >> 16) & 0xFF;

    // 选择 LBA 模式
    __asm__ volatile("mov $0x1F6, %%dx\n mov $0xE0, %%al\n out %%al, %%dx" : : : "al", "dx");
    // 扇区数 = 1
    __asm__ volatile("mov $0x1F2, %%dx\n mov $0x01, %%al\n out %%al, %%dx" : : : "al", "dx");
    // LBA 低 8 位
    __asm__ volatile("mov $0x1F3, %%dx\n mov %0, %%al\n out %%al, %%dx" : : "a" (lba_low) : "dx");
    // LBA 中 8 位
    __asm__ volatile("mov $0x1F4, %%dx\n mov %0, %%al\n out %%al, %%dx" : : "a" (lba_mid) : "dx");
    // LBA 高 8 位
    __asm__ volatile("mov $0x1F5, %%dx\n mov %0, %%al\n out %%al, %%dx" : : "a" (lba_high) : "dx");
    // 写命令
    __asm__ volatile("mov $0x1F7, %%dx\n mov $0x30, %%al\n out %%al, %%dx" : : : "al", "dx");

    ata_wait_ready();  // ← 等待写入命令接受完成
    for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
        uint16_t word = buffer[i*2] | (buffer[i*2+1] << 8);
        __asm__ volatile(
            "mov $0x1F0, %%dx\n"
            "mov %0, %%ax\n"
            "out %%ax, %%dx"
            : : "a"(word) : "dx"
        );
    }
    ata_wait_ready();
}
struct fat12_bpb {
    uint8_t  jump[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media_descriptor;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t  drive_number;
    uint8_t  reserved;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  filesystem_type[8];
} __attribute__((packed));

struct fat12_dir_entry {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attributes;
    uint8_t  reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_low;
    uint32_t file_size;
} __attribute__((packed));
// 挂载 FAT12（从 LBA 1 开始，跳过 MBR）
void fat12_mount() {
    uint8_t boot_sector[FAT12_SECTOR_SIZE];
    ata_read_sector(0, boot_sector);
    struct fat12_bpb* bpb = (struct fat12_bpb*)boot_sector;
    
    // 放宽检测条件
    // 检查是否可能是 FAT12（通过 bytes_per_sector 和 sectors_per_fat）
    if (bpb->bytes_per_sector != 512 || bpb->sectors_per_fat == 0) {
        print("Not a valid FAT12 filesystem.\n");
        return;
    }
    
    // 如果 boot_signature 不是 0x29，也尝试挂载（兼容性）
    // 但保留警告
    if (bpb->boot_signature != 0x29) {
        print("Warning: non-standard boot signature, attempting to mount anyway.\n");
    }
    
    fat12_reserved_sectors = bpb->reserved_sectors;
    fat12_sectors_per_fat = bpb->sectors_per_fat;
    fat12_root_dir_sector = bpb->reserved_sectors + (bpb->fat_count * fat12_sectors_per_fat);
    fat12_data_sector = fat12_root_dir_sector + (bpb->root_entries * 32 / FAT12_SECTOR_SIZE);
    fat12_mounted = 1;
    print("FAT12 filesystem mounted.\n");
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

// ---------- 路径工具函数 ----------
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

// ---------- 文件系统初始化 ----------
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
// ---------- FAT12 辅助函数 ----------

// 读取根目录到内存（用于遍历）
void fat12_read_root_dir(uint8_t* buffer) {
    for (int i = 0; i < 14; i++) {
        ata_read_sector(fat12_root_dir_sector + i, buffer + i * FAT12_SECTOR_SIZE);
    }
}

// 写入根目录（保存修改）
void fat12_write_root_dir(const uint8_t* buffer) {
    for (int i = 0; i < 14; i++) {
        ata_write_sector(fat12_root_dir_sector + i, buffer + i * FAT12_SECTOR_SIZE);
    }
}

// 查找根目录中的文件/目录项，返回索引
int fat12_find_entry(const char* filename, struct fat12_dir_entry* entry_out) {
    uint8_t root_dir[FAT12_SECTOR_SIZE * 14];
    fat12_read_root_dir(root_dir);
    struct fat12_dir_entry* entries = (struct fat12_dir_entry*)root_dir;
    
    // 将文件名转换为 8.3 格式
    char name_83[11];
    int i, j;
    for (i = 0; i < 11; i++) name_83[i] = ' ';
    for (i = 0; filename[i] && i < 8; i++) {
        if (filename[i] == '.') break;
        name_83[i] = filename[i];
    }
    if (filename[i] == '.') {
        i++;
        for (j = 0; filename[i + j] && j < 3; j++) {
            name_83[8 + j] = filename[i + j];
        }
    }
    
    for (int i = 0; i < FAT12_ROOT_ENTRIES; i++) {
        if (entries[i].name[0] == 0x00) break;
        if (entries[i].name[0] == 0xE5) continue;
        // 比较名字（8 字节）
        int match = 1;
        for (int j = 0; j < 11; j++) {
            if (entries[i].name[j] != name_83[j]) { match = 0; break; }
        }
        if (match) {
            if (entry_out) *entry_out = entries[i];
            return i;
        }
    }
    return -1;
}

// 读取文件数据（根据簇链）
void fat12_read_file(struct fat12_dir_entry* entry, uint8_t* buffer) {
    if (entry->file_size == 0) return;
    uint16_t cluster = entry->cluster_low;
    int bytes_read = 0;
    while (cluster < 0xFF8 && bytes_read < entry->file_size) {
        uint32_t sector = fat12_data_sector + (cluster - 2) * FAT12_SECTORS_PER_CLUSTER;
        ata_read_sector(sector, buffer + bytes_read);
        bytes_read += FAT12_SECTOR_SIZE;
        // 读取 FAT 表找下一个簇
        uint8_t fat_sector[FAT12_SECTOR_SIZE];
        uint32_t fat_offset = cluster * 3 / 2;
        ata_read_sector(fat12_reserved_sectors + fat_offset / FAT12_SECTOR_SIZE, fat_sector);
        uint16_t fat_entry;
        if (cluster % 2 == 0) {
            fat_entry = fat_sector[fat_offset % FAT12_SECTOR_SIZE] | 
                        ((fat_sector[fat_offset % FAT12_SECTOR_SIZE + 1] & 0x0F) << 8);
        } else {
            fat_entry = ((fat_sector[fat_offset % FAT12_SECTOR_SIZE] & 0xF0) >> 4) |
                        (fat_sector[fat_offset % FAT12_SECTOR_SIZE + 1] << 4);
        }
        cluster = fat_entry;
    }
}

// 写入文件数据（简单实现：只支持写入第一个簇）
void fat12_write_file(struct fat12_dir_entry* entry, const uint8_t* data, int size) {
    if (size == 0) return;
    uint16_t cluster = entry->cluster_low;
    if (cluster == 0) {
        // 分配第一个簇（从 2 开始查找）
        cluster = 2;
        // 更新 FAT 表标记此簇已使用
    }
    uint32_t sector = fat12_data_sector + (cluster - 2) * FAT12_SECTORS_PER_CLUSTER;
    // 写入数据（补齐到 512 字节）
    uint8_t sector_data[FAT12_SECTOR_SIZE];
    for (int i = 0; i < FAT12_SECTOR_SIZE; i++) {
        sector_data[i] = (i < size) ? data[i] : 0;
    }
    ata_write_sector(sector, sector_data);
    entry->file_size = size;
}
// ---------- FAT12 目录操作 ----------

// 读取目录内容（根目录或子目录）
void fat12_read_dir(uint16_t cluster, uint8_t* buffer) {
    if (cluster == 0) {
        // 根目录
        fat12_read_root_dir(buffer);
    } else {
        // 子目录：读取簇
        uint32_t sector = fat12_data_sector + (cluster - 2) * FAT12_SECTORS_PER_CLUSTER;
        ata_read_sector(sector, buffer);
    }
}

// 写入目录内容
void fat12_write_dir(uint16_t cluster, const uint8_t* buffer) {
    if (cluster == 0) {
        fat12_write_root_dir(buffer);
    } else {
        uint32_t sector = fat12_data_sector + (cluster - 2) * FAT12_SECTORS_PER_CLUSTER;
        ata_write_sector(sector, buffer);
    }
}

// 在指定目录中查找文件/目录项
int fat12_find_entry_in_dir(uint16_t cluster, const char* filename, struct fat12_dir_entry* entry_out) {
    // 使用全局缓冲区（已定义为 uint8_t fat12_dir_buffer[FAT12_SECTOR_SIZE * 14]）
    uint8_t* dir_buf = fat12_dir_buffer;
    fat12_read_dir(cluster, dir_buf);
    struct fat12_dir_entry* entries = (struct fat12_dir_entry*)dir_buf;

    char name_83[11];
    int i, j;
    for (i = 0; i < 11; i++) name_83[i] = ' ';
    for (i = 0; filename[i] && i < 8; i++) {
        if (filename[i] == '.') break;
        name_83[i] = filename[i];
    }
    if (filename[i] == '.') {
        i++;
        for (j = 0; filename[i + j] && j < 3; j++) {
            name_83[8 + j] = filename[i + j];
        }
    }

    for (int i = 0; i < 16; i++) {  // 一个扇区可容纳 16 个目录项（512/32）
        if (entries[i].name[0] == 0x00) break;
        if (entries[i].name[0] == 0xE5) continue;
        int match = 1;
        for (int j = 0; j < 11; j++) {
            if (entries[i].name[j] != name_83[j]) { match = 0; break; }
        }
        if (match) {
            if (entry_out) *entry_out = entries[i];
            return i;
        }
    }
    return -1;
}

// 获取当前目录的簇号（0 表示根目录）
uint16_t fat12_get_current_cluster() {
    return current_fat_cluster;
}

// 设置当前目录
void fat12_set_current_cluster(uint16_t cluster) {
    current_fat_cluster = cluster;
}
// ---------- 格式化 FAT12 ----------
void fat12_format() {
    print("Formatting FAT12 filesystem...\n");
    // 写入引导扇区
    uint8_t boot_sector[FAT12_SECTOR_SIZE] = {0};
    struct fat12_bpb* bpb = (struct fat12_bpb*)boot_sector;
    bpb->jump[0] = 0xEB; bpb->jump[1] = 0x3C; bpb->jump[2] = 0x90;
    bpb->oem[0] = 'M'; bpb->oem[1] = 'S'; bpb->oem[2] = 'W';
    bpb->oem[3] = 'I'; bpb->oem[4] = 'N'; bpb->oem[5] = '4';
    bpb->oem[6] = '.'; bpb->oem[7] = '1';
    bpb->bytes_per_sector = 512;
    bpb->sectors_per_cluster = 1;
    bpb->reserved_sectors = 1;
    bpb->fat_count = 2;
    bpb->root_entries = 224;
    bpb->total_sectors_16 = 65535;
    bpb->media_descriptor = 0xF8;
    bpb->sectors_per_fat = 9;
    bpb->sectors_per_track = 18;
    bpb->heads = 2;
    bpb->hidden_sectors = 0;
    bpb->total_sectors_32 = 0;
    bpb->drive_number = 0x80;
    bpb->reserved = 0;
    bpb->boot_signature = 0x29;
    bpb->volume_id = 0x12345678;
    bpb->volume_label[0] = 'N'; bpb->volume_label[1] = 'O'; bpb->volume_label[2] = 'V'; bpb->volume_label[3] = 'A';
    bpb->volume_label[4] = ' '; bpb->volume_label[5] = 'O'; bpb->volume_label[6] = 'S';
    bpb->volume_label[7] = ' '; bpb->volume_label[8] = ' '; bpb->volume_label[9] = ' '; bpb->volume_label[10] = ' ';
    bpb->filesystem_type[0] = 'F'; bpb->filesystem_type[1] = 'A'; bpb->filesystem_type[2] = 'T';
    bpb->filesystem_type[3] = '1'; bpb->filesystem_type[4] = '2'; bpb->filesystem_type[5] = ' ';
    bpb->filesystem_type[6] = ' '; bpb->filesystem_type[7] = ' ';
    boot_sector[510] = 0x55;
    boot_sector[511] = 0xAA;
    ata_write_sector(0, boot_sector);
    
    // 清空 FAT 表（2 份）
    uint8_t zero_sector[FAT12_SECTOR_SIZE] = {0};
    for (int i = 0; i < 9; i++) {
        ata_write_sector(1 + i, zero_sector);
        ata_write_sector(1 + 9 + i, zero_sector);
    }
    // 标记 FAT 前两个簇为已用
    zero_sector[0] = 0xF8; zero_sector[1] = 0xFF; zero_sector[2] = 0xFF;
    ata_write_sector(1, zero_sector);
    ata_write_sector(1 + 9, zero_sector);
    
    // 清空根目录（14 个扇区）
    for (int i = 0; i < 14; i++) {
        ata_write_sector(fat12_root_dir_sector + i, zero_sector);
    }
    fat12_reserved_sectors = 1;
    fat12_sectors_per_fat = 9;
    fat12_root_dir_sector = 1 + 2 * 9;  // reserved + fat_count * sectors_per_fat
    fat12_data_sector = fat12_root_dir_sector + (224 * 32 / 512);  // root_dir + root_entries * 32 / 512
    fat12_mounted = 1;

    
}
// ---------- 处理命令 ----------
static void process_command() {
    // -------- ls --------
    if (strcmp(cmd_buffer, "ls") == 1) {
        if (!fat12_mounted) { print("No FAT12 filesystem.\n"); goto done; }
        uint8_t* dir_buf = fat12_dir_buffer;
        fat12_read_dir(current_fat_cluster, dir_buf);
        struct fat12_dir_entry* entries = (struct fat12_dir_entry*)dir_buf;
        int count = 0;
        for (int i = 0; i < 16; i++) {
            if (entries[i].name[0] == 0x00) break;
            if (entries[i].name[0] == 0xE5) continue;
            if (entries[i].attributes & 0x10) {
                print("[DIR]  ");
            } else {
                print("[FILE] ");
            }
            char filename[13];
            int j;
            for (j = 0; j < 8 && entries[i].name[j] != ' '; j++) filename[j] = entries[i].name[j];
            if (entries[i].attributes & 0x10) {
                // 目录：直接结束，不加点
                filename[j] = '\0';
            } else {
                // 文件：添加点+扩展名
                filename[j] = '.';
                for (int k = 0; k < 3 && entries[i].ext[k] != ' '; k++) {
                    filename[j+1+k] = entries[i].ext[k];
                }
                filename[j+1+3] = '\0';
            }
            print(filename);
            print("  ");
            char buf[16];
            print(itoa(entries[i].file_size, buf));
            print(" bytes\n");
            count++;
        }
        if (count == 0) print("(empty)\n");
    }
    // -------- mkdir --------
    else if (starts_with(cmd_buffer, "mkdir ")) {
        if (!fat12_mounted) { print("No FAT12 filesystem.\n"); goto done; }
        const char* dname = cmd_buffer + 6;
        while (*dname == ' ') dname++;
        if (*dname == '\0') {
            print("Error: missing directory name\n");
            goto done;
        }
        // 检查是否已存在
        struct fat12_dir_entry entry;
        if (fat12_find_entry_in_dir(current_fat_cluster, dname, &entry) != -1) {
            print("Error: already exists\n");
            goto done;
        }
        // 读取当前目录
        uint8_t* dir_buf = fat12_dir_buffer;
        fat12_read_dir(current_fat_cluster, dir_buf);
        struct fat12_dir_entry* entries = (struct fat12_dir_entry*)dir_buf;
        int slot = -1;
        for (int i = 0; i < 16; i++) {
            if (entries[i].name[0] == 0x00 || entries[i].name[0] == 0xE5) {
                slot = i;
                break;
            }
        }
        if (slot == -1) { print("Error: directory full\n"); goto done; }
        // 分配一个簇
        uint16_t new_cluster = 2;
        // 查找空闲簇（简单实现：从 2 开始顺序查找）
        for (uint16_t c = 2; c < 0xFF0; c++) {
            uint8_t fat_sector[FAT12_SECTOR_SIZE];
            ata_read_sector(1, fat_sector);
            uint16_t fat_entry;
            uint32_t offset = c * 3 / 2;
            if (c % 2 == 0) {
                fat_entry = fat_sector[offset % FAT12_SECTOR_SIZE] |
                            ((fat_sector[offset % FAT12_SECTOR_SIZE + 1] & 0x0F) << 8);
            } else {
                fat_entry = ((fat_sector[offset % FAT12_SECTOR_SIZE] & 0xF0) >> 4) |
                            (fat_sector[offset % FAT12_SECTOR_SIZE + 1] << 4);
            }
            if (fat_entry == 0x000) {
                new_cluster = c;
                break;
            }
        }
        // 标记簇为已用（0xFFF）
        uint8_t fat_sector[FAT12_SECTOR_SIZE];
        ata_read_sector(1, fat_sector);
        uint32_t offset = new_cluster * 3 / 2;
        if (new_cluster % 2 == 0) {
            fat_sector[offset % FAT12_SECTOR_SIZE] = 0xFF;
            fat_sector[offset % FAT12_SECTOR_SIZE + 1] = (fat_sector[offset % FAT12_SECTOR_SIZE + 1] & 0xF0) | 0x0F;
        } else {
            fat_sector[offset % FAT12_SECTOR_SIZE] = (fat_sector[offset % FAT12_SECTOR_SIZE] & 0x0F) | 0xF0;
            fat_sector[offset % FAT12_SECTOR_SIZE + 1] = 0xFF;
        }
        ata_write_sector(1, fat_sector);
        // 第二份 FAT
        ata_write_sector(1 + 9, fat_sector);

        // 初始化目录项
        for (int i = 0; i < 11; i++) entries[slot].name[i] = ' ';
        for (int i = 0; dname[i] && i < 8; i++) {
            if (dname[i] == '.') break;
            entries[slot].name[i] = dname[i];
        }
        entries[slot].ext[0] = ' '; entries[slot].ext[1] = ' '; entries[slot].ext[2] = ' ';
        entries[slot].attributes = 0x10;  // 目录标记
        entries[slot].cluster_low = new_cluster;
        entries[slot].file_size = 0;
        fat12_write_dir(current_fat_cluster, dir_buf);

        // 在新目录中写入 "." 和 ".." 目录项
        uint8_t new_dir_buf[FAT12_SECTOR_SIZE] = {0};
        struct fat12_dir_entry* new_entries = (struct fat12_dir_entry*)new_dir_buf;
        // "."
        for (int i = 0; i < 11; i++) new_entries[0].name[i] = ' ';
        new_entries[0].name[0] = '.';
        new_entries[0].attributes = 0x10;
        new_entries[0].cluster_low = new_cluster;
        // ".."
        for (int i = 0; i < 11; i++) new_entries[1].name[i] = ' ';
        new_entries[1].name[0] = '.'; new_entries[1].name[1] = '.';
        new_entries[1].attributes = 0x10;
        new_entries[1].cluster_low = current_fat_cluster;

        uint32_t sector = fat12_data_sector + (new_cluster - 2) * FAT12_SECTORS_PER_CLUSTER;
        ata_write_sector(sector, new_dir_buf);
        print("Directory created.\n");
    }
   // -------- cd --------
    else if (starts_with(cmd_buffer, "cd ")) {
        if (!fat12_mounted) { print("No FAT12 filesystem.\n"); goto done; }
        const char* path = cmd_buffer + 3;
        while (*path == ' ') path++;
        if (*path == '\0') {
            current_fat_cluster = 0;
            strcpy_safe(current_fat_path, "/", MAX_PATH);
            print("Changed to: /\n");
            goto done;
        }
        if (strcmp(path, "..") == 1) {
            // 返回上级目录
            if (current_fat_cluster != 0) {
                // 读取当前目录的 ".." 项
                uint8_t* dir_buf = fat12_dir_buffer;
                fat12_read_dir(current_fat_cluster, dir_buf);
                struct fat12_dir_entry* entries = (struct fat12_dir_entry*)dir_buf;
                current_fat_cluster = entries[1].cluster_low;  // ".." 的簇号
                // 更新路径显示
                // 简化：直接显示 "/"
                print("Changed to: /\n");
            } else {
                print("Already at root.\n");
            }
            goto done;
        }
        struct fat12_dir_entry entry;
        int idx = fat12_find_entry_in_dir(current_fat_cluster, path, &entry);
        if (idx == -1 || !(entry.attributes & 0x10)) {
            print("Error: directory not found\n");
            goto done;
        }
        current_fat_cluster = entry.cluster_low;
        print("Changed to: ");
        print(path);
        print("\n");
    }
    // -------- pwd --------
    else if (strcmp(cmd_buffer, "pwd") == 1) {
        if (!fat12_mounted) { print("No FAT12 filesystem.\n"); goto done; }
        if (current_fat_cluster == 0) {
            print("/\n");
        } else {
            // 简单显示："/subdir"
            print("(path tracking simplified)\n");
        }
    }
    // -------- touch --------
    else if (starts_with(cmd_buffer, "touch ")) {
        if (!fat12_mounted) { print("No FAT12 filesystem.\n"); goto done; }
        const char* fname = cmd_buffer + 6;
        while (*fname == ' ') fname++;
        if (*fname == '\0') { print("Error: missing filename\n"); goto done; }
        // 检查是否已存在（当前目录）
        struct fat12_dir_entry entry;
        if (fat12_find_entry_in_dir(current_fat_cluster, fname, &entry) != -1) {
            print("File already exists.\n");
            goto done;
        }
        // 读取当前目录
        uint8_t* dir_buf = fat12_dir_buffer;
        fat12_read_dir(current_fat_cluster, dir_buf);
        struct fat12_dir_entry* entries = (struct fat12_dir_entry*)dir_buf;
        // 查找空闲槽位
        int slot = -1;
        for (int i = 0; i < 16; i++) {
            if (entries[i].name[0] == 0x00 || entries[i].name[0] == 0xE5) {
                slot = i;
                break;
            }
        }
        if (slot == -1) { print("Error: directory full\n"); goto done; }
        // 初始化目录项
        for (int i = 0; i < 11; i++) entries[slot].name[i] = ' ';
        for (int i = 0; fname[i] && i < 8; i++) {
            if (fname[i] == '.') break;
            entries[slot].name[i] = fname[i];
        }
        int dot_pos = 0;
        for (int i = 0; fname[i]; i++) { if (fname[i] == '.') { dot_pos = i; break; } }
        if (dot_pos > 0) {
            for (int i = dot_pos + 1, j = 0; fname[i] && j < 3; i++, j++) {
                entries[slot].ext[j] = fname[i];
            }
        }
        entries[slot].attributes = 0x00;      // 普通文件
        entries[slot].cluster_low = 0;        // 无数据
        entries[slot].file_size = 0;
        fat12_write_dir(current_fat_cluster, dir_buf);
        print("File created.\n");
    }
    // -------- cat --------
    else if (starts_with(cmd_buffer, "cat ")) {
        if (!fat12_mounted) { print("No FAT12 filesystem.\n"); goto done; }
        const char* fname = cmd_buffer + 4;
        while (*fname == ' ') fname++;
        if (*fname == '\0') { print("Error: missing filename\n"); goto done; }
        struct fat12_dir_entry entry;
        if (fat12_find_entry_in_dir(current_fat_cluster, fname, &entry) == -1) {
            print("File not found.\n");
            goto done;
        }
        if (entry.file_size == 0) { print("(empty file)\n"); goto done; }
        if (entry.cluster_low == 0) { print("(empty file)\n"); goto done; }
        uint8_t data[entry.file_size + 1];
        uint32_t sector = fat12_data_sector + (entry.cluster_low - 2) * FAT12_SECTORS_PER_CLUSTER;
        ata_read_sector(sector, data);
        data[entry.file_size] = '\0';
        print((char*)data);
        print("\n");
    }
   // -------- write --------
    else if (starts_with(cmd_buffer, "write ")) {
        if (!fat12_mounted) { print("No FAT12 filesystem.\n"); goto done; }
        const char* rest = cmd_buffer + 6;
        while (*rest == ' ') rest++;
        char fname[MAX_FILENAME];
        int i = 0;
        while (*rest != ' ' && *rest != '\0') { fname[i++] = *rest++; }
        fname[i] = '\0';
        while (*rest == ' ') rest++;
        if (*rest == '\0') { print("Error: no content\n"); goto done; }
        struct fat12_dir_entry entry;
        int idx = fat12_find_entry_in_dir(current_fat_cluster, fname, &entry);
        if (idx == -1) { print("File not found.\n"); goto done; }
        int len = 0;
        const char* p = rest;
        while (*p++) len++;
        if (len > 0) len--;
        if (len > FAT12_SECTOR_SIZE) { print("Error: content too large (max 512 bytes)\n"); goto done; }
        uint8_t* dir_buf = fat12_dir_buffer;
        fat12_read_dir(current_fat_cluster, dir_buf);
        struct fat12_dir_entry* entries = (struct fat12_dir_entry*)dir_buf;

        // 如果文件没有分配簇，分配一个
        if (entry.cluster_low == 0) {
            uint16_t new_cluster = 2;
            for (uint16_t c = 2; c < 0xFF0; c++) {
                uint8_t fat_sector[FAT12_SECTOR_SIZE];
                ata_read_sector(1, fat_sector);
                uint16_t fat_entry;
                uint32_t offset = c * 3 / 2;
                if (c % 2 == 0) {
                    fat_entry = fat_sector[offset % FAT12_SECTOR_SIZE] |
                                ((fat_sector[offset % FAT12_SECTOR_SIZE + 1] & 0x0F) << 8);
                } else {
                    fat_entry = ((fat_sector[offset % FAT12_SECTOR_SIZE] & 0xF0) >> 4) |
                                (fat_sector[offset % FAT12_SECTOR_SIZE + 1] << 4);
                }
                if (fat_entry == 0x000) {
                    new_cluster = c;
                    break;
                }
            }
            // 标记 FAT 表（两份）
            uint8_t fat_sector[FAT12_SECTOR_SIZE];
            ata_read_sector(1, fat_sector);
            uint32_t offset = new_cluster * 3 / 2;
            if (new_cluster % 2 == 0) {
                fat_sector[offset % FAT12_SECTOR_SIZE] = 0xFF;
                fat_sector[offset % FAT12_SECTOR_SIZE + 1] = (fat_sector[offset % FAT12_SECTOR_SIZE + 1] & 0xF0) | 0x0F;
            } else {
                fat_sector[offset % FAT12_SECTOR_SIZE] = (fat_sector[offset % FAT12_SECTOR_SIZE] & 0x0F) | 0xF0;
                fat_sector[offset % FAT12_SECTOR_SIZE + 1] = 0xFF;
            }
            ata_write_sector(1, fat_sector);
            ata_write_sector(1 + 9, fat_sector);

            // ★★★ 关键：更新目录项的 cluster_low ★★★
            entries[idx].cluster_low = new_cluster;
        }

        // 写入数据到数据区
        uint32_t sector = fat12_data_sector + (entries[idx].cluster_low - 2) * FAT12_SECTORS_PER_CLUSTER;
        uint8_t sector_data[FAT12_SECTOR_SIZE];
        for (int j = 0; j < FAT12_SECTOR_SIZE; j++) {
            sector_data[j] = (j < len) ? rest[j] : 0;
        }
        ata_write_sector(sector, sector_data);
        entries[idx].file_size = len;

        //关键：保存目录项到磁盘
        fat12_write_dir(current_fat_cluster, dir_buf);

        char buf[16];
        print("Written ");
        print(itoa(len, buf));
        print(" bytes.\n");
    }
    // -------- rm --------
    else if (starts_with(cmd_buffer, "rm ")) {
        if (!fat12_mounted) { print("No FAT12 filesystem.\n"); goto done; }
        const char* fname = cmd_buffer + 3;
        while (*fname == ' ') fname++;
        if (*fname == '\0') { print("Error: missing filename\n"); goto done; }
        struct fat12_dir_entry entry;
        int idx = fat12_find_entry_in_dir(current_fat_cluster, fname, &entry);
        if (idx == -1) { print("File not found.\n"); goto done; }
        // 如果是目录，检查是否为空
        if (entry.attributes & 0x10) {
            // 检查目录是否为空（除了 . 和 ..）
            uint8_t* dir_buf = fat12_dir_buffer;
            fat12_read_dir(entry.cluster_low, dir_buf);
            struct fat12_dir_entry* sub_entries = (struct fat12_dir_entry*)dir_buf;
            int empty = 1;
            for (int i = 2; i < 16; i++) {  // 跳过 . 和 ..
                if (sub_entries[i].name[0] != 0x00 && sub_entries[i].name[0] != 0xE5) {
                    empty = 0;
                    break;
                }
            }
            if (!empty) { print("Error: directory not empty\n"); goto done; }
            // 释放目录的簇
            uint8_t fat_sector[FAT12_SECTOR_SIZE];
            ata_read_sector(1, fat_sector);
            uint32_t offset = entry.cluster_low * 3 / 2;
            if (entry.cluster_low % 2 == 0) {
                fat_sector[offset % FAT12_SECTOR_SIZE] = 0x00;
                fat_sector[offset % FAT12_SECTOR_SIZE + 1] = fat_sector[offset % FAT12_SECTOR_SIZE + 1] & 0xF0;
            } else {
                fat_sector[offset % FAT12_SECTOR_SIZE] = fat_sector[offset % FAT12_SECTOR_SIZE] & 0x0F;
                fat_sector[offset % FAT12_SECTOR_SIZE + 1] = 0x00;
            }
            ata_write_sector(1, fat_sector);
            ata_write_sector(1 + 9, fat_sector);
        } else {
            // 文件：释放簇
            if (entry.cluster_low != 0) {
                uint8_t fat_sector[FAT12_SECTOR_SIZE];
                ata_read_sector(1, fat_sector);
                uint32_t offset = entry.cluster_low * 3 / 2;
                if (entry.cluster_low % 2 == 0) {
                    fat_sector[offset % FAT12_SECTOR_SIZE] = 0x00;
                    fat_sector[offset % FAT12_SECTOR_SIZE + 1] = fat_sector[offset % FAT12_SECTOR_SIZE + 1] & 0xF0;
                } else {
                    fat_sector[offset % FAT12_SECTOR_SIZE] = fat_sector[offset % FAT12_SECTOR_SIZE] & 0x0F;
                    fat_sector[offset % FAT12_SECTOR_SIZE + 1] = 0x00;
                }
                ata_write_sector(1, fat_sector);
                ata_write_sector(1 + 9, fat_sector);
            }
        }
        // 标记目录项为删除
        uint8_t dir_buf[FAT12_SECTOR_SIZE];
        fat12_read_dir(current_fat_cluster, dir_buf);
        struct fat12_dir_entry* entries = (struct fat12_dir_entry*)dir_buf;
        entries[idx].name[0] = 0xE5;
        fat12_write_dir(current_fat_cluster, dir_buf);
        print("Deleted.\n");
    }
    // -------- format --------
    else if (strcmp(cmd_buffer, "format") == 1) {
        fat12_format();
        print("FAT12 formatted.\n");
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
        print("Tip: Use ↑/↓ to browse command history\n");
    }else if (strcmp(cmd_buffer, "reboot") == 1) {
        print("Rebooting...\n");
        __asm__ volatile("int $0x19");
    }else if (strcmp(cmd_buffer, "version") == 1) {
        print("Nova OS Kernel v0.6 (History)\n");
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
    // ---------- 保存命令到历史 ----------
    if (cmd_buffer[0] != '\0') {
        strcpy_safe(history[history_tail], cmd_buffer, CMD_BUFFER_SIZE);
        history_tail = (history_tail + 1) % HISTORY_SIZE;
        if (history_count < HISTORY_SIZE) history_count++;
    }
    history_pos = 0;  // 重置浏览位置

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

    // ---------- 上箭头 (0x48) - 命令历史向上 ----------
    if (scancode == 0x48) {
        if (history_count > 0) {
            // 如果当前没有浏览历史，从最新开始
            if (history_pos == 0) {
                history_pos = history_tail == 0 ? HISTORY_SIZE - 1 : history_tail - 1;
                if (history_count < HISTORY_SIZE) {
                    history_pos = history_count - 1;
                }
            } else {
                int prev = (history_pos - 1 + HISTORY_SIZE) % HISTORY_SIZE;
                if (prev != history_tail || history_count == HISTORY_SIZE) {
                    history_pos = prev;
                }
            }
            // 回显历史命令
            while (cmd_index > 0) {
                backspace();
                cmd_index--;
            }
            char* cmd = history[history_pos];
            int i = 0;
            while (cmd[i]) {
                putchar(cmd[i]);
                cmd_buffer[i] = cmd[i];
                i++;
            }
            cmd_buffer[i] = '\0';
            cmd_index = i;
        }
        __asm__ volatile("mov $0x20, %%al\n out %%al, $0x20" : : : "eax");
        disable_hardware_cursor();
        return;
    }

    // ---------- 下箭头 (0x50) - 命令历史向下 ----------
    if (scancode == 0x50) {
        if (history_pos != 0 && history_count > 0) {
            int next = (history_pos + 1) % HISTORY_SIZE;
            if (next != history_tail || history_count == HISTORY_SIZE) {
                history_pos = next;
            } else {
                history_pos = 0;  // 退出历史模式
            }
            // 回显命令
            while (cmd_index > 0) {
                backspace();
                cmd_index--;
            }
            if (history_pos != 0) {
                char* cmd = history[history_pos];
                int i = 0;
                while (cmd[i]) {
                    putchar(cmd[i]);
                    cmd_buffer[i] = cmd[i];
                    i++;
                }
                cmd_buffer[i] = '\0';
                cmd_index = i;
            } else {
                cmd_buffer[0] = '\0';
                cmd_index = 0;
            }
        }
        __asm__ volatile("mov $0x20, %%al\n out %%al, $0x20" : : : "eax");
        disable_hardware_cursor();
        return;
    }

    // ---------- 退格 ----------
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
    fat12_format();
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
    print("           Nova OS v0.6      \n");
    fat12_mount();  // 尝试挂载 FAT12
    print("Commands: ls, mkdir, cd, pwd, touch, cat, write, rm, cls, help, reboot, version, echo, beep, shutdown, calc\n");
    print("> ");
    __asm__ volatile("sti");

    while (1) {
        // 空闲
    }
}