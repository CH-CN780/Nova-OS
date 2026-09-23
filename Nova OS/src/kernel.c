#include <stdint.h>

// ================== 前置声明 ==================
void  putchar(char c);
void  clear_screen(void);
void  update_cursor(void);
void  scroll(void);
void  backspace(void);
void  print(const char* str);
void  disable_hardware_cursor(void);
char* itoa(int num, char* buffer);
int   atoi(const char* str);
int   strcmp(const char* a, const char* b);
int   starts_with(const char* str, const char* prefix);
void  strcpy_safe(char* dest, const char* src, int max_len);

int      ata_read_sector(uint32_t lba, uint8_t* buffer);
int      ata_write_sector(uint32_t lba, const uint8_t* buffer);
uint32_t ata_disk_sectors(void);
static int allow_install = 0; 
int  fat32_mount(void);
void fat32_format(uint32_t total_sectors);
extern void mouse_handler_wrapper();
extern void exception_handler_wrapper();

void exception_handler_c(void) {
    print("\n*** CPU EXCEPTION - system halted ***\n");
    print("A command triggered a CPU fault.\n");
    print("Reboot the VM to continue.\n");
    while (1) __asm__ volatile("cli; hlt");
}
void mouse_init(void);
void mouse_handler_c(void);
void mouse_disable(void);
void mouse_enable(void);
int  mouse_is_enabled(void);

// ================== 全局变量 ==================
// ===== 链接脚本导出的符号（linker.ld 里定义） =====
extern uint8_t _kernel_start[];
extern uint8_t _kernel_file_end[];
extern uint8_t _bss_start[];
extern uint8_t _bss_end[];
extern uint8_t _kernel_entry[];
volatile int cursor_x = 0;
volatile int cursor_y = 0;
volatile int interrupt_counter = 0;
volatile int shift_pressed = 0;

static int old_cursor_x = 0;
static int old_cursor_y = 0;
static char saved_char = ' ';
static uint8_t saved_attr = 0x07;
static int cursor_visible = 1;

#define CMD_BUFFER_SIZE 256
static char cmd_buffer[CMD_BUFFER_SIZE];
static int cmd_index = 0;

// ================== 大缓冲区（5MB，cat / write / cxx 共用） ==================
#define BIG_BUF_SIZE (5 * 1024 * 1024)
static uint8_t big_buffer[BIG_BUF_SIZE + 1];
// ===== objcopy 嵌入的引导器二进制符号 =====
// objcopy 输出的符号名固定是 _binary_<文件名>_start / _end
// 用 __asm__ 显式绑定，避免 Windows/Linux 下 C 符号前缀差异
extern unsigned char mbr_bin[]        __asm__("_binary_mbr_bin_start");
extern unsigned char mbr_bin_end[]    __asm__("_binary_mbr_bin_end");
extern unsigned char stage2_bin[]     __asm__("_binary_stage2_bin_start");
extern unsigned char stage2_bin_end[] __asm__("_binary_stage2_bin_end");

#define mbr_bin_len    ((unsigned int)(mbr_bin_end    - mbr_bin))
#define stage2_bin_len ((unsigned int)(stage2_bin_end - stage2_bin))
// ================== 行输入（write 模式） ==================
static volatile int line_reading = 0;
static volatile int line_ready   = 0;
static char line_buffer[CMD_BUFFER_SIZE];

// ================== cat 查看器状态 ==================
static uint32_t cat_file_size = 0;
static int      cat_total_lines = 0;
static volatile int cat_scroll_line = 0;
static volatile int cat_dirty = 0;

// ================== PS/2 鼠标 ==================
static int mouse_enabled = 1;
static volatile int cat_viewing = 0;
static int mouse_px = 320;
static int mouse_py = 240;
static int mouse_wheel_mode = 0;

#define HISTORY_SIZE 16
static char history[HISTORY_SIZE][CMD_BUFFER_SIZE];
static int history_count = 0;
static int history_pos = 0;
static int history_tail = 0;
#define FAT32_VOLUME_OFFSET 2048

// ================== 批处理状态 ==================
static int batch_mode = 0;
static int batch_depth = 0;
#define BATCH_MAX_DEPTH 8

// ================== ATA PIO 驱动（LBA28） ==================
#define ATA_SECTOR_SIZE 512

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t r;
    __asm__ volatile("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static void kbc_flush_output(void) {
    uint32_t timeout = 100000;
    while (timeout--) {
        uint8_t st = inb(0x64);
        if (!(st & 0x01)) return;
        inb(0x60);
    }
}

static void ata_delay_us(int us) {
    for (int i = 0; i < us; i++) inb(0x80);
}
static void ata_delay_ms(int ms) {
    for (int i = 0; i < ms; i++) ata_delay_us(1000);
}
static void ata_io_delay(void) {
    inb(0x3F6); inb(0x3F6); inb(0x3F6); inb(0x3F6);
}
static void ata_wait_bsy(void) {
    uint32_t timeout = 200000;
    while ((inb(0x1F7) & 0x80) && timeout--) { }
}
static int ata_wait_drq(void) {
    uint32_t timeout = 200000;
    while (timeout--) {
        uint8_t s = inb(0x1F7);
        if (s & 0x80) continue;
        if (s & 0x08) return 1;
        if (s & 0x01) return 0;
    }
    return 0;
}
static void ata_soft_reset(void) {
    outb(0x3F6, 0x06);
    ata_delay_us(5);
    outb(0x3F6, 0x02);
    ata_delay_ms(10);
    ata_wait_bsy();
}

static int ata_flush_suppress = 0;
static void ata_flush(void) {
    outb(0x1F7, 0xE7);
    ata_wait_bsy();
}

int ata_read_sector(uint32_t lba, uint8_t* buffer) {
    ata_wait_bsy();
    outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    ata_io_delay();
    outb(0x1F2, 1);
    outb(0x1F3, lba & 0xFF);
    outb(0x1F4, (lba >> 8)  & 0xFF);
    outb(0x1F5, (lba >> 16) & 0xFF);
    outb(0x1F7, 0x20);
    ata_wait_bsy();
    if (!ata_wait_drq()) { ata_soft_reset(); return 0; }
    for (int i = 0; i < 256; i++) {
        uint16_t w = inw(0x1F0);
        buffer[i*2]   = w & 0xFF;
        buffer[i*2+1] = (w >> 8) & 0xFF;
    }
    return 1;
}

int ata_write_sector(uint32_t lba, const uint8_t* buffer) {
    ata_wait_bsy();
    outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    ata_io_delay();
    outb(0x1F2, 1);
    outb(0x1F3, lba & 0xFF);
    outb(0x1F4, (lba >> 8)  & 0xFF);
    outb(0x1F5, (lba >> 16) & 0xFF);
    outb(0x1F7, 0x30);
    ata_wait_bsy();
    if (!ata_wait_drq()) {
        uint8_t st = inb(0x1F7);
        uint8_t er = inb(0x1F1);
        print("ATA write ERR: status=");
        char b[8]; print(itoa(st, b));
        print(" err="); print(itoa(er, b));
        print(" lba="); print(itoa((int)lba, b)); print("\n");
        ata_soft_reset();
        return 0;
    }
    for (int i = 0; i < 256; i++) {
        uint16_t w = buffer[i*2] | (buffer[i*2+1] << 8);
        outw(0x1F0, w);
    }
    if (!ata_flush_suppress) ata_flush();
    return 1;
}

static uint32_t g_disk_sectors = 0;
uint32_t ata_disk_sectors(void) { return g_disk_sectors; }

static uint32_t ata_identify_sectors(void) {
    for (int attempt = 0; attempt < 3; attempt++) {
        ata_delay_ms(50);
        ata_soft_reset();
        ata_wait_bsy();
        outb(0x1F6, 0xA0);
        ata_delay_us(5);
        outb(0x1F2, 0); outb(0x1F3, 0);
        outb(0x1F4, 0); outb(0x1F5, 0);
        outb(0x1F7, 0xEC);
        uint8_t st = inb(0x1F7);
        if (st == 0) continue;
        uint32_t timeout = 10000000;
        int ready = 0;
        while (timeout--) {
            st = inb(0x1F7);
            if (st & 0x80) continue;
            if (st & 0x01) break;
            if (st & 0x08) { ready = 1; break; }
        }
        if (!ready) continue;
        uint16_t data[256];
        for (int i = 0; i < 256; i++) data[i] = inw(0x1F0);
        uint32_t sectors = (uint32_t)data[60] | ((uint32_t)data[61] << 16);
        if (sectors > 0 && sectors < 0xFFFFFFF0) return sectors;
    }
    return 0;
}

// ================== FAT32 数据结构 ==================
struct fat32_bpb {
    uint8_t  jump[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media_descriptor;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved2;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  filesystem_type[8];
} __attribute__((packed));

struct fat32_dir_entry {
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

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F
#define FAT32_EOC      0x0FFFFFF8
#define FAT32_FREE     0x00000000
#define FAT32_BAD      0x0FFFFFF7

static uint32_t fat32_sectors_per_fat;
static uint32_t fat32_root_cluster;
static uint32_t fat32_fat_start;
static uint32_t fat32_data_start;
static uint32_t fat32_total_sectors;
static uint32_t fat32_spc;
static uint32_t fat32_bpc;
static uint32_t fat32_cur_cluster;
static int fat32_mounted = 0;
static char fat32_cur_path[256] = "/";

static inline uint32_t dir_cluster(const struct fat32_dir_entry* e) {
    return ((uint32_t)e->cluster_high << 16) | e->cluster_low;
}
static inline void dir_set_cluster(struct fat32_dir_entry* e, uint32_t c) {
    e->cluster_high = (uint16_t)(c >> 16);
    e->cluster_low  = (uint16_t)(c & 0xFFFF);
}
static uint32_t fat32_cluster_sector(uint32_t cluster) {
    return fat32_data_start + (cluster - 2) * fat32_spc;
}
static uint32_t fat32_get_fat(uint32_t cluster) {
    uint32_t off = cluster * 4;
    uint8_t buf[ATA_SECTOR_SIZE];
    if (!ata_read_sector(fat32_fat_start + (off / ATA_SECTOR_SIZE), buf)) return FAT32_EOC;
    uint32_t v = *(uint32_t*)(buf + (off % ATA_SECTOR_SIZE));
    return v & 0x0FFFFFFF;
}
static void fat32_put_fat(uint32_t cluster, uint32_t value) {
    uint32_t off = cluster * 4;
    uint32_t sec = fat32_fat_start + (off / ATA_SECTOR_SIZE);
    uint8_t buf[ATA_SECTOR_SIZE];
    if (!ata_read_sector(sec, buf)) return;
    uint32_t* p = (uint32_t*)(buf + (off % ATA_SECTOR_SIZE));
    *p = (*p & 0xF0000000) | (value & 0x0FFFFFFF);
    if (!ata_write_sector(sec, buf)) return;
    ata_write_sector(sec + fat32_sectors_per_fat, buf);
}
static uint32_t fat32_alloc_cluster(void) {
    for (uint32_t c = 2; c < FAT32_EOC; c++) {
        if (fat32_get_fat(c) == FAT32_FREE) {
            fat32_put_fat(c, 0x0FFFFFFF);
            return c;
        }
    }
    return 0;
}
static void fat32_free_chain(uint32_t cluster) {
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t next = fat32_get_fat(cluster);
        fat32_put_fat(cluster, FAT32_FREE);
        cluster = next;
    }
}

int fat32_mount(void) {
    uint8_t boot[ATA_SECTOR_SIZE];
    if (!ata_read_sector(FAT32_VOLUME_OFFSET, boot)) return 0;
    if (boot[510] != 0x55 || boot[511] != 0xAA) return 0;
    struct fat32_bpb* bpb = (struct fat32_bpb*)boot;
    if (bpb->bytes_per_sector != 512) return 0;
    if (bpb->sectors_per_fat_32 == 0) return 0;
    if (bpb->sectors_per_cluster == 0 || bpb->fat_count == 0 || bpb->reserved_sectors == 0) return 0;
    fat32_sectors_per_fat = bpb->sectors_per_fat_32;
    fat32_root_cluster    = bpb->root_cluster;
    fat32_fat_start       = FAT32_VOLUME_OFFSET + bpb->reserved_sectors;
    fat32_data_start      = FAT32_VOLUME_OFFSET + bpb->reserved_sectors
                          + bpb->fat_count * fat32_sectors_per_fat;
    fat32_total_sectors   = bpb->total_sectors_32 ? bpb->total_sectors_32 : bpb->total_sectors_16;
    fat32_spc             = bpb->sectors_per_cluster;
    fat32_bpc             = fat32_spc * ATA_SECTOR_SIZE;
    fat32_cur_cluster     = fat32_root_cluster;
    fat32_mounted         = 1;
    print("FAT32 mounted. Cluster size: ");
    char buf[16];
    print(itoa((int)fat32_bpc, buf));
    print(" bytes, Volume: ");
    print(itoa((int)((uint32_t)fat32_total_sectors * 512 / (1024*1024)), buf));
    print(" MB\n");
    return 1;
}

void fat32_format(uint32_t total_sectors) {
    ata_flush_suppress = 1;
    if (total_sectors < 0x10000) { print("FAT32: volume too small.\n"); goto done; }
    print("Formatting FAT32...\n");
    uint32_t spc = 8, reserved = 32, fat_count = 2;
    uint32_t fat_sectors = 256;
    for (int i = 0; i < 4; i++) {
        uint32_t data_sectors = total_sectors - reserved - fat_count * fat_sectors;
        uint32_t clusters = data_sectors / spc;
        fat_sectors = (clusters * 4 + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE + 1;
    }
    uint8_t boot[ATA_SECTOR_SIZE];
    for (int i = 0; i < ATA_SECTOR_SIZE; i++) boot[i] = 0;
    struct fat32_bpb* bpb = (struct fat32_bpb*)boot;
    bpb->jump[0] = 0xEB; bpb->jump[1] = 0x58; bpb->jump[2] = 0x90;
    const char* oem = "NOVAOS  ";
    for (int i = 0; i < 8; i++) bpb->oem[i] = oem[i];
    bpb->bytes_per_sector    = 512;
    bpb->sectors_per_cluster = (uint8_t)spc;
    bpb->reserved_sectors    = (uint16_t)reserved;
    bpb->fat_count           = (uint8_t)fat_count;
    bpb->root_entries        = 0;
    bpb->total_sectors_16    = 0;
    bpb->media_descriptor    = 0xF8;
    bpb->sectors_per_fat_16  = 0;
    bpb->sectors_per_track   = 63;
    bpb->heads               = 255;
    bpb->hidden_sectors      = FAT32_VOLUME_OFFSET;
    bpb->total_sectors_32    = total_sectors;
    bpb->sectors_per_fat_32  = fat_sectors;
    bpb->ext_flags           = 0;
    bpb->fs_version          = 0;
    bpb->root_cluster        = 2;
    bpb->fsinfo_sector       = 1;
    bpb->backup_boot_sector  = 6;
    bpb->drive_number        = 0x80;
    bpb->boot_signature      = 0x29;
    bpb->volume_id           = 0x4E4F5641;
    const char* label = "NOVA       ";
    for (int i = 0; i < 11; i++) bpb->volume_label[i] = label[i];
    const char* fstype = "FAT32   ";
    for (int i = 0; i < 8; i++) bpb->filesystem_type[i] = fstype[i];
    boot[510] = 0x55; boot[511] = 0xAA;
    if (!ata_write_sector(FAT32_VOLUME_OFFSET + 0, boot)) { print("FAT32: boot write failed\n"); goto done; }
    uint8_t zero[ATA_SECTOR_SIZE];
    for (int i = 0; i < ATA_SECTOR_SIZE; i++) zero[i] = 0;
    for (uint32_t i = 1; i < reserved; i++)
        if (!ata_write_sector(FAT32_VOLUME_OFFSET + i, zero)) { print("FAT32: reserved write failed\n"); goto done; }
    uint8_t fat1[ATA_SECTOR_SIZE];
    for (int i = 0; i < ATA_SECTOR_SIZE; i++) fat1[i] = 0;
    fat1[0]=0xF8; fat1[1]=0xFF; fat1[2]=0xFF; fat1[3]=0x0F;
    fat1[4]=0xFF; fat1[5]=0xFF; fat1[6]=0xFF; fat1[7]=0x0F;
    fat1[8]=0xFF; fat1[9]=0xFF; fat1[10]=0xFF; fat1[11]=0x0F;
    uint32_t fat1_start = reserved;
    uint32_t fat2_start = reserved + fat_sectors;
    if (!ata_write_sector(FAT32_VOLUME_OFFSET + fat1_start, fat1) ||
        !ata_write_sector(FAT32_VOLUME_OFFSET + fat2_start, fat1)) {
        print("FAT32: FAT header write failed\n"); goto done;
    }
    for (int i = 0; i < ATA_SECTOR_SIZE; i++) fat1[i] = 0;
    for (uint32_t i = 1; i < fat_sectors; i++) {
        if (!ata_write_sector(FAT32_VOLUME_OFFSET + fat1_start + i, fat1) ||
            !ata_write_sector(FAT32_VOLUME_OFFSET + fat2_start + i, fat1)) {
            print("FAT32: FAT body write failed\n"); goto done;
        }
    }
    uint32_t data_start = reserved + fat_count * fat_sectors;
    for (uint32_t s = 0; s < spc; s++)
        if (!ata_write_sector(FAT32_VOLUME_OFFSET + data_start + s, zero)) { print("FAT32: root cluster write failed\n"); goto done; }
    fat32_sectors_per_fat = fat_sectors;
    fat32_root_cluster    = 2;
    fat32_fat_start       = FAT32_VOLUME_OFFSET + reserved;
    fat32_data_start      = FAT32_VOLUME_OFFSET + data_start;
    fat32_total_sectors   = total_sectors;
    fat32_spc             = spc;
    fat32_bpc             = spc * ATA_SECTOR_SIZE;
    fat32_cur_cluster     = 2;
    fat32_mounted         = 1;
    print("FAT32 formatted. sectors_per_fat=");
    char b[16];
    print(itoa((int)fat_sectors, b));
    print("\n");
done:
    ata_flush_suppress = 0;
    ata_flush();
}

int fat32_find_in_dir(uint32_t dir_cluster, const char* name,
                      struct fat32_dir_entry* out_entry,
                      uint32_t* out_sector, int* out_index) {
    char name83[11];
    for (int i = 0; i < 11; i++) name83[i] = ' ';
    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && i < 8) { name83[i] = name[i]; i++; }
    if (name[i] == '.') {
        i++;
        while (name[i] && j < 3) { name83[8 + j] = name[i]; i++; j++; }
    }
    uint32_t cluster = dir_cluster;
    uint8_t buf[ATA_SECTOR_SIZE];
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t base = fat32_cluster_sector(cluster);
        for (uint32_t s = 0; s < fat32_spc; s++) {
            uint32_t sector = base + s;
            if (!ata_read_sector(sector, buf)) return -1;
            struct fat32_dir_entry* entries = (struct fat32_dir_entry*)buf;
            for (int idx = 0; idx < 16; idx++) {
                if (entries[idx].name[0] == 0x00) return -1;
                if (entries[idx].name[0] == 0xE5) continue;
                if (entries[idx].attributes == ATTR_LFN) continue;
                if (entries[idx].attributes & ATTR_VOLUME_ID) continue;
                int match = 1;
                for (int k = 0; k < 11; k++)
                    if (entries[idx].name[k] != name83[k]) { match = 0; break; }
                if (match) {
                    if (out_entry)  *out_entry = entries[idx];
                    if (out_sector) *out_sector = sector;
                    if (out_index)  *out_index = idx;
                    return 0;
                }
            }
        }
        cluster = fat32_get_fat(cluster);
    }
    return -1;
}

int fat32_create_in_dir(uint32_t dir_cluster, const char* name,
                        uint8_t attr, uint32_t first_cluster, uint32_t size) {
    char name83[11];
    for (int i = 0; i < 11; i++) name83[i] = ' ';
    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && i < 8) { name83[i] = name[i]; i++; }
    if (name[i] == '.') {
        i++;
        while (name[i] && j < 3) { name83[8 + j] = name[i]; i++; j++; }
    }
    uint32_t cluster = dir_cluster;
    uint32_t prev = 0;
    uint8_t buf[ATA_SECTOR_SIZE];
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t base = fat32_cluster_sector(cluster);
        for (uint32_t s = 0; s < fat32_spc; s++) {
            uint32_t sector = base + s;
            if (!ata_read_sector(sector, buf)) return -1;
            struct fat32_dir_entry* entries = (struct fat32_dir_entry*)buf;
            for (int idx = 0; idx < 16; idx++) {
                if (entries[idx].name[0] == 0x00 || entries[idx].name[0] == 0xE5) {
                    for (int k = 0; k < 8; k++) entries[idx].name[k] = name83[k];
                    for (int k = 0; k < 3; k++) entries[idx].ext[k]  = name83[8+k];
                    entries[idx].attributes        = attr;
                    entries[idx].reserved          = 0;
                    entries[idx].create_time_tenth = 0;
                    entries[idx].create_time       = 0;
                    entries[idx].create_date       = 0;
                    entries[idx].access_date       = 0;
                    entries[idx].cluster_high      = (uint16_t)(first_cluster >> 16);
                    entries[idx].modify_time       = 0;
                    entries[idx].modify_date       = 0;
                    entries[idx].cluster_low       = (uint16_t)(first_cluster & 0xFFFF);
                    entries[idx].file_size         = size;
                    if (!ata_write_sector(sector, buf)) { print("FAT32: dir write failed\n"); return -1; }
                    uint8_t verify[ATA_SECTOR_SIZE];
                    if (!ata_read_sector(sector, verify)) { print("FAT32: verify read failed\n"); return -1; }
                    if (((struct fat32_dir_entry*)verify)[idx].name[0] != name83[0]) { print("FAT32: verify mismatch\n"); return -1; }
                    return 0;
                }
            }
        }
        prev = cluster;
        cluster = fat32_get_fat(cluster);
    }
    uint32_t new_c = fat32_alloc_cluster();
    if (new_c == 0) return -1;
    fat32_put_fat(prev, new_c);
    fat32_put_fat(new_c, 0x0FFFFFFF);
    for (int k = 0; k < ATA_SECTOR_SIZE; k++) buf[k] = 0;
    struct fat32_dir_entry* e = (struct fat32_dir_entry*)buf;
    for (int k = 0; k < 8; k++) e->name[k] = name83[k];
    for (int k = 0; k < 3; k++) e->ext[k]  = name83[8+k];
    e->attributes   = attr;
    e->cluster_high = (uint16_t)(first_cluster >> 16);
    e->cluster_low  = (uint16_t)(first_cluster & 0xFFFF);
    e->file_size    = size;
    uint32_t base = fat32_cluster_sector(new_c);
    if (!ata_write_sector(base, buf)) return -1;
    for (int k = 0; k < ATA_SECTOR_SIZE; k++) buf[k] = 0;
    for (uint32_t s = 1; s < fat32_spc; s++) ata_write_sector(base + s, buf);
    return 0;
}

uint32_t fat32_read_file(const struct fat32_dir_entry* entry,
                         uint8_t* buffer, uint32_t max_size) {
    uint32_t size = entry->file_size;
    if (size > max_size) size = max_size;
    uint32_t cluster = dir_cluster(entry);
    uint32_t read = 0;
    uint8_t buf[ATA_SECTOR_SIZE];
    while (cluster >= 2 && cluster < FAT32_EOC && read < size) {
        uint32_t base = fat32_cluster_sector(cluster);
        for (uint32_t s = 0; s < fat32_spc && read < size; s++) {
            if (!ata_read_sector(base + s, buf)) return read;
            uint32_t to_copy = size - read;
            if (to_copy > ATA_SECTOR_SIZE) to_copy = ATA_SECTOR_SIZE;
            for (uint32_t k = 0; k < to_copy; k++) buffer[read + k] = buf[k];
            read += to_copy;
        }
        cluster = fat32_get_fat(cluster);
    }
    return read;
}

int fat32_overwrite_file(uint32_t dir_sector, int dir_index,
                         const uint8_t* data, uint32_t size) {
    uint8_t dbuf[ATA_SECTOR_SIZE];
    if (!ata_read_sector(dir_sector, dbuf)) return -1;
    struct fat32_dir_entry* entries = (struct fat32_dir_entry*)dbuf;
    struct fat32_dir_entry* e = &entries[dir_index];
    uint32_t old = dir_cluster(e);
    if (old >= 2 && old < FAT32_EOC) fat32_free_chain(old);
    uint32_t first = 0, prev = 0;
    uint32_t written = 0;
    uint8_t buf[ATA_SECTOR_SIZE];
    while (written < size) {
        uint32_t c = fat32_alloc_cluster();
        if (c == 0) return -1;
        if (first == 0) first = c;
        if (prev != 0) fat32_put_fat(prev, c);
        prev = c;
        fat32_put_fat(c, 0x0FFFFFFF);
        uint32_t base = fat32_cluster_sector(c);
        for (uint32_t s = 0; s < fat32_spc; s++) {
            for (int k = 0; k < ATA_SECTOR_SIZE; k++)
                buf[k] = (written < size) ? data[written++] : 0;
            if (!ata_write_sector(base + s, buf)) return -1;
            if (written >= size) break;
        }
    }
    if (!ata_read_sector(dir_sector, dbuf)) return -1;
    entries = (struct fat32_dir_entry*)dbuf;
    dir_set_cluster(&entries[dir_index], first);
    entries[dir_index].file_size = size;
    if (!ata_write_sector(dir_sector, dbuf)) return -1;
    return 0;
}

// ================== 屏幕 / 光标 ==================
void disable_hardware_cursor(void) {
    __asm__ volatile(
        "mov $0x0A, %%al\n out %%al, $0x3D4\n"
        "mov $0x1F, %%al\n out %%al, $0x3D5\n"
        "mov $0x0B, %%al\n out %%al, $0x3D4\n"
        "mov $0x00, %%al\n out %%al, $0x3D5\n"
        : : : "al", "dx", "memory");
    __asm__ volatile(
        "mov $0x0E, %%al\n out %%al, $0x3D4\n"
        "mov $0x07, %%al\n out %%al, $0x3D5\n"
        "mov $0x0F, %%al\n out %%al, $0x3D4\n"
        "mov $0xD0, %%al\n out %%al, $0x3D5\n"
        : : : "al", "dx", "memory");
}
void erase_cursor(void) {
    if (cursor_visible) {
        char* video = (char*)0xB8000;
        int pos = old_cursor_y * 80 + old_cursor_x;
        video[2*pos] = saved_char;
        video[2*pos+1] = saved_attr;
        cursor_visible = 0;
    }
}
void draw_cursor(void) {
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
void update_cursor(void) {
    if (old_cursor_x != cursor_x || old_cursor_y != cursor_y) erase_cursor();
    draw_cursor();
    disable_hardware_cursor();
}
void clear_screen(void) {
    char* video = (char*)0xB8000;
    for (int i = 0; i < 80 * 25; i++) { video[2*i] = ' '; video[2*i+1] = 0x07; }
    cursor_x = 0; cursor_y = 0;
    old_cursor_x = 0; old_cursor_y = 0;
    saved_char = ' '; saved_attr = 0x07;
    cursor_visible = 0;
    disable_hardware_cursor();
    update_cursor();
}
void scroll(void) {
    erase_cursor();
    char* video = (char*)0xB8000;
    for (int row = 1; row < 25; row++)
        for (int col = 0; col < 80; col++) {
            int src = row * 80 + col;
            int dst = (row - 1) * 80 + col;
            video[2*dst] = video[2*src];
            video[2*dst+1] = video[2*src+1];
        }
    for (int col = 0; col < 80; col++) {
        int pos = 24 * 80 + col;
        video[2*pos] = ' ';
        video[2*pos+1] = 0x07;
    }
    cursor_y = 24; cursor_x = 0;
    update_cursor();
}
void putchar(char c) {
    erase_cursor();
    char* video = (char*)0xB8000;
    if (c == '\n') { cursor_x = 0; cursor_y++; }
    else if (c == '\r') { cursor_x = 0; }
    else {
        int pos = cursor_y * 80 + cursor_x;
        video[2*pos] = c;
        video[2*pos+1] = 0x07;
        cursor_x++;
        if (cursor_x >= 80) { cursor_x = 0; cursor_y++; }
    }
    if (cursor_y >= 25) scroll();
    else update_cursor();
}
void print(const char* str) { while (*str) { putchar(*str); str++; } }
void backspace(void) {
    erase_cursor();
    if (cursor_x > 0) cursor_x--;
    else if (cursor_y > 0) { cursor_y--; cursor_x = 79; }
    else { update_cursor(); return; }
    char* video = (char*)0xB8000;
    int pos = cursor_y * 80 + cursor_x;
    video[2*pos] = ' ';
    video[2*pos+1] = 0x07;
    update_cursor();
}

// ================== 字符串工具 ==================
int strcmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == *b);
}
int starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++; prefix++;
    }
    return 1;
}
int atoi(const char* str) {
    int result = 0, sign = 1;
    while (*str == ' ') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') { result = result * 10 + (*str - '0'); str++; }
    return result * sign;
}
char* itoa(int num, char* buffer) {
    char* ptr = buffer;
    int neg = 0;
    if (num < 0) { neg = 1; num = -num; }
    char temp[16];
    int i = 0;
    if (num == 0) temp[i++] = '0';
    while (num > 0) { temp[i++] = '0' + (num % 10); num /= 10; }
    if (neg) temp[i++] = '-';
    while (i > 0) *ptr++ = temp[--i];
    *ptr = '\0';
    return buffer;
}
void strcpy_safe(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}
static void format_name_83(const struct fat32_dir_entry* e, char* out) {
    int p = 0;
    for (int i = 0; i < 8 && e->name[i] != ' '; i++) out[p++] = e->name[i];
    int has_ext = 0;
    for (int i = 0; i < 3; i++) if (e->ext[i] != ' ') has_ext = 1;
    if (has_ext) {
        out[p++] = '.';
        for (int i = 0; i < 3 && e->ext[i] != ' '; i++) out[p++] = e->ext[i];
    }
    out[p] = '\0';
}

// ================== 迷你 C++ 编译器 (x86-32 JIT) ==================
// 直接生成 x86 机器码到可执行缓冲区，然后跳转执行

__attribute__((force_align_arg_pointer))
static void cxx_rt_print_int(int v) {
    char b[16];
    itoa(v, b);
    print(b);
}
__attribute__((force_align_arg_pointer))
static void cxx_rt_print_str(const char* s) {
    print(s);
}
__attribute__((force_align_arg_pointer))
static void cxx_rt_print_nl(void) {
    putchar('\n');
}

#define CXX_CODE_SIZE    (64 * 1024)
#define CXX_MAX_LOCALS   64
#define CXX_STRPOOL_SIZE 8192

static uint8_t cxx_code[CXX_CODE_SIZE];
static int     cxx_cp = 0;
static int     cxx_ccerr = 0;
static const char* cxx_ccerrmsg = "";

static struct { char name[32]; int disp; } cxx_locals[CXX_MAX_LOCALS];
static int cxx_nlocals = 0;
static int cxx_frame   = 0;

static char cxx_strpool[CXX_STRPOOL_SIZE];
static int  cxx_strpool_used = 0;

static const char* cxx_sp = 0;
static volatile int cxx_running = 0;

static void cxx_e8(uint8_t b) {
    if (cxx_cp < CXX_CODE_SIZE) cxx_code[cxx_cp++] = b;
    else cxx_ccerr = 1;
}
static void cxx_e32(uint32_t v) {
    cxx_e8((uint8_t)(v));
    cxx_e8((uint8_t)(v >> 8));
    cxx_e8((uint8_t)(v >> 16));
    cxx_e8((uint8_t)(v >> 24));
}
static void cxx_fail(const char* msg) {
    if (!cxx_ccerr) { cxx_ccerr = 1; cxx_ccerrmsg = msg; }
}

// ------ x86 指令封装 ------
static void g_mov_eax_imm(uint32_t v) { cxx_e8(0xB8); cxx_e32(v); }
static void g_mov_eax_local(int d)    { cxx_e8(0x8B); cxx_e8(0x85); cxx_e32((uint32_t)d); }
static void g_mov_local_eax(int d)    { cxx_e8(0x89); cxx_e8(0x85); cxx_e32((uint32_t)d); }
static void g_push_eax(void)          { cxx_e8(0x50); }
static void g_pop_ebx(void)           { cxx_e8(0x5B); }
static void g_push_imm(uint32_t v)    { cxx_e8(0x68); cxx_e32(v); }
static void g_add_eax_ebx(void)       { cxx_e8(0x01); cxx_e8(0xD8); }
static void g_sub_eax_ebx(void)       { cxx_e8(0x29); cxx_e8(0xD8); }
static void g_imul_eax_ebx(void)      { cxx_e8(0x0F); cxx_e8(0xAF); cxx_e8(0xC3); }
static void g_cdq(void)               { cxx_e8(0x99); }
static void g_idiv_ebx(void)          { cxx_e8(0xF7); cxx_e8(0xFB); }
static void g_mov_eax_edx(void)       { cxx_e8(0x89); cxx_e8(0xD0); }
static void g_cmp_ebx_eax(void)       { cxx_e8(0x39); cxx_e8(0xC3); }
static void g_test_eax(void)          { cxx_e8(0x85); cxx_e8(0xC0); }
static void g_neg_eax(void)           { cxx_e8(0xF7); cxx_e8(0xD8); }
static void g_xchg_eax_ebx(void)      { cxx_e8(0x93); }
static void g_add_esp_4(void)         { cxx_e8(0x83); cxx_e8(0xC4); cxx_e8(0x04); }
static void g_or_eax_ebx(void)        { cxx_e8(0x09); cxx_e8(0xD8); }
static void g_and_eax_ebx(void)       { cxx_e8(0x21); cxx_e8(0xD8); }
static void g_movzx_eax_al(void)      { cxx_e8(0x0F); cxx_e8(0xB6); cxx_e8(0xC0); }
static void g_setcc_al(int cc)        { cxx_e8(0x0F); cxx_e8((uint8_t)cc); cxx_e8(0xC0); }
static void g_leave(void)             { cxx_e8(0xC9); }
static void g_ret(void)               { cxx_e8(0xC3); }
static void g_push_ebp(void)          { cxx_e8(0x55); }
static void g_mov_ebp_esp(void)       { cxx_e8(0x89); cxx_e8(0xE5); }

static int g_sub_esp_placeholder(void) {
    cxx_e8(0x81); cxx_e8(0xEC);
    int p = cxx_cp; cxx_e32(0); return p;
}
static void g_patch_sub_esp(int p, int n) {
    cxx_code[p]   = n & 0xFF;
    cxx_code[p+1] = (n >> 8) & 0xFF;
    cxx_code[p+2] = (n >> 16) & 0xFF;
    cxx_code[p+3] = (n >> 24) & 0xFF;
}
static void g_normalize_eax(void) {
    g_test_eax(); g_setcc_al(0x95); g_movzx_eax_al();
}
static void g_normalize_ebx(void) {
    cxx_e8(0x85); cxx_e8(0xDB);
    cxx_e8(0x0F); cxx_e8(0x95); cxx_e8(0xC3);
    cxx_e8(0x0F); cxx_e8(0xB6); cxx_e8(0xDB);
}
static void g_call(uint32_t target) {
    cxx_e8(0xE8);
    uint32_t next_ip = (uint32_t)(uintptr_t)(cxx_code + cxx_cp + 4);
    int32_t  rel     = (int32_t)(target - next_ip);
    cxx_e32((uint32_t)rel);
}
static int g_jcc(int cc) {
    cxx_e8(0x0F); cxx_e8((uint8_t)cc);
    int p = cxx_cp; cxx_e32(0); return p;
}
static int g_jmp(void) {
    cxx_e8(0xE9);
    int p = cxx_cp; cxx_e32(0); return p;
}
static void g_patch(int p) {
    uint32_t rel = (uint32_t)cxx_cp - (uint32_t)(p + 4);
    cxx_code[p]   = rel & 0xFF;
    cxx_code[p+1] = (rel >> 8) & 0xFF;
    cxx_code[p+2] = (rel >> 16) & 0xFF;
    cxx_code[p+3] = (rel >> 24) & 0xFF;
}

// ------ 词法/语法 ------
static int cxx_is_alpha(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int cxx_is_digit(char c) { return c>='0'&&c<='9'; }
static int cxx_is_alnum(char c) { return cxx_is_alpha(c)||cxx_is_digit(c); }

static void cxx_skip_ws(void) {
    for (;;) {
        while (*cxx_sp==' '||*cxx_sp=='\t'||*cxx_sp=='\r'||*cxx_sp=='\n') cxx_sp++;
        if (cxx_sp[0]=='/' && cxx_sp[1]=='/') {
            while (*cxx_sp && *cxx_sp!='\n') cxx_sp++;
            continue;
        }
        if (cxx_sp[0]=='/' && cxx_sp[1]=='*') {
            cxx_sp += 2;
            while (*cxx_sp && !(cxx_sp[0]=='*'&&cxx_sp[1]=='/')) cxx_sp++;
            if (*cxx_sp) cxx_sp += 2;
            continue;
        }
        if (*cxx_sp == '#') {
            while (*cxx_sp && *cxx_sp != '\n') cxx_sp++;
            continue;
        }
        break;
    }
}
static int cxx_peek_kw(const char* kw) {
    int n=0; while (kw[n]) n++;
    for (int i=0;i<n;i++) if (cxx_sp[i]!=kw[i]) return 0;
    if (cxx_is_alnum(cxx_sp[n])) return 0;
    return 1;
}
static int cxx_take_kw(const char* kw) {
    if (!cxx_peek_kw(kw)) return 0;
    int n=0; while (kw[n]) n++;
    cxx_sp += n;
    return 1;
}

static int cxx_lookup(const char* name) {
    for (int i=0;i<cxx_nlocals;i++)
        if (strcmp(cxx_locals[i].name, name)==1) return cxx_locals[i].disp;
    return 0;
}
static int cxx_newlocal(const char* name) {
    if (cxx_nlocals >= CXX_MAX_LOCALS) { cxx_fail("too many locals"); return -4; }
    cxx_frame += 4;
    strcpy_safe(cxx_locals[cxx_nlocals].name, name, 32);
    cxx_locals[cxx_nlocals].disp = -cxx_frame;
    cxx_nlocals++;
    return -cxx_frame;
}

static void c_expr(void);
static void cxx_compile_stmt(void);

static void c_primary(void) {
    cxx_skip_ws();
    if (*cxx_sp=='(') {
        cxx_sp++; c_expr(); cxx_skip_ws();
        if (*cxx_sp==')') cxx_sp++; else cxx_fail("expected )");
        return;
    }
    if (*cxx_sp=='-') { cxx_sp++; c_primary(); g_neg_eax(); return; }
    if (*cxx_sp=='+') { cxx_sp++; c_primary(); return; }
    if (*cxx_sp=='!') {
        cxx_sp++; c_primary();
        g_test_eax(); g_setcc_al(0x94); g_movzx_eax_al();
        return;
    }
    if (cxx_is_digit(*cxx_sp)) {
        int v = 0;
        while (cxx_is_digit(*cxx_sp)) { v = v*10 + (*cxx_sp - '0'); cxx_sp++; }
        g_mov_eax_imm((uint32_t)v);
        return;
    }
    if (cxx_is_alpha(*cxx_sp)) {
        char name[32]; int n=0;
        while (cxx_is_alnum(*cxx_sp)) { if (n<31) name[n++]=*cxx_sp; cxx_sp++; }
        name[n]='\0';
        int d = cxx_lookup(name);
        if (d == 0) { cxx_fail("undefined variable"); return; }
        g_mov_eax_local(d);
        return;
    }
    cxx_fail("bad primary");
}

static void c_mul(void) {
    c_primary();
    while (!cxx_ccerr) {
        cxx_skip_ws();
        char c = *cxx_sp;
        if (c=='*' && cxx_sp[1] != '=') {
            cxx_sp++; g_push_eax(); c_primary(); g_pop_ebx();
            g_imul_eax_ebx();
        } else if (c=='/') {
            cxx_sp++; g_push_eax(); c_primary(); g_pop_ebx();
            g_xchg_eax_ebx(); g_cdq(); g_idiv_ebx();
        } else if (c=='%') {
            cxx_sp++; g_push_eax(); c_primary(); g_pop_ebx();
            g_xchg_eax_ebx(); g_cdq(); g_idiv_ebx(); g_mov_eax_edx();
        } else break;
    }
}
static void c_add(void) {
    c_mul();
    while (!cxx_ccerr) {
        cxx_skip_ws();
        char c = *cxx_sp;
        if (c=='+') { cxx_sp++; g_push_eax(); c_mul(); g_pop_ebx(); g_add_eax_ebx(); }
        else if (c=='-') { cxx_sp++; g_push_eax(); c_mul(); g_pop_ebx(); g_xchg_eax_ebx(); g_sub_eax_ebx(); }
        else break;
    }
}
static void c_cmp(void) {
    c_add();
    while (!cxx_ccerr) {
        cxx_skip_ws();
        int cc = -1;
        if      (cxx_sp[0]=='=' && cxx_sp[1]=='=') { cc=0x94; cxx_sp+=2; }
        else if (cxx_sp[0]=='!' && cxx_sp[1]=='=') { cc=0x95; cxx_sp+=2; }
        else if (cxx_sp[0]=='<' && cxx_sp[1]=='=') { cc=0x9E; cxx_sp+=2; }
        else if (cxx_sp[0]=='>' && cxx_sp[1]=='=') { cc=0x9D; cxx_sp+=2; }
        else if (cxx_sp[0]=='<') { cc=0x9C; cxx_sp+=1; }
        else if (cxx_sp[0]=='>') { cc=0x9F; cxx_sp+=1; }
        else break;
        g_push_eax(); c_add(); g_pop_ebx();
        g_cmp_ebx_eax();
        g_setcc_al(cc); g_movzx_eax_al();
    }
}
static void c_and(void) {
    c_cmp();
    while (!cxx_ccerr) {
        cxx_skip_ws();
        if (cxx_sp[0]=='&' && cxx_sp[1]=='&') {
            cxx_sp += 2;
            g_push_eax(); c_cmp(); g_pop_ebx();
            g_xchg_eax_ebx();
            g_normalize_eax(); g_normalize_ebx();
            g_and_eax_ebx();
        } else break;
    }
}
static void c_or(void) {
    c_and();
    while (!cxx_ccerr) {
        cxx_skip_ws();
        if (cxx_sp[0]=='|' && cxx_sp[1]=='|') {
            cxx_sp += 2;
            g_push_eax(); c_and(); g_pop_ebx();
            g_xchg_eax_ebx();
            g_normalize_eax(); g_normalize_ebx();
            g_or_eax_ebx();
        } else break;
    }
}
static void c_expr(void) { c_or(); }

static void cxx_compile_stmt(void) {
    cxx_skip_ws();
    if (!*cxx_sp) return;
    if (*cxx_sp == '{') {
        cxx_sp++;
        while (!cxx_ccerr) {
            cxx_skip_ws();
            if (*cxx_sp == '}') { cxx_sp++; return; }
            if (!*cxx_sp) { cxx_fail("unclosed {"); return; }
            cxx_compile_stmt();
        }
        return;
    }
    if (*cxx_sp == ';') { cxx_sp++; return; }

    if (cxx_take_kw("return")) {
        cxx_skip_ws();
        if (*cxx_sp == ';') { g_mov_eax_imm(0); cxx_sp++; }
        else {
            c_expr(); cxx_skip_ws();
            if (*cxx_sp==';') cxx_sp++; else cxx_fail("expected ;");
        }
        g_leave(); g_ret();
        return;
    }

    if (cxx_take_kw("int") || cxx_take_kw("long")) {
        cxx_skip_ws();
        char name[32]; int n=0;
        while (cxx_is_alnum(*cxx_sp)) { if (n<31) name[n++]=*cxx_sp; cxx_sp++; }
        name[n]='\0';
        if (n==0) { cxx_fail("expected name"); return; }
        int d = cxx_newlocal(name);
        cxx_skip_ws();
        if (*cxx_sp=='=' && cxx_sp[1] != '=') { cxx_sp++; c_expr(); g_mov_local_eax(d); }
        cxx_skip_ws();
        if (*cxx_sp==';') cxx_sp++; else cxx_fail("expected ; after declaration");
        return;
    }

    if (cxx_take_kw("if")) {
        cxx_skip_ws();
        if (*cxx_sp != '(') { cxx_fail("expected ( after if"); return; }
        cxx_sp++; c_expr(); cxx_skip_ws();
        if (*cxx_sp != ')') { cxx_fail("expected ) after if"); return; }
        cxx_sp++;
        g_test_eax();
        int jz_pos = g_jcc(0x84);
        cxx_compile_stmt();
        int jmp_pos = g_jmp();
        g_patch(jz_pos);
        cxx_skip_ws();
        if (cxx_take_kw("else")) cxx_compile_stmt();
        g_patch(jmp_pos);
        return;
    }

    if (cxx_take_kw("while")) {
        int loop_start = cxx_cp;
        cxx_skip_ws();
        if (*cxx_sp != '(') { cxx_fail("expected ( after while"); return; }
        cxx_sp++; c_expr(); cxx_skip_ws();
        if (*cxx_sp != ')') { cxx_fail("expected ) after while"); return; }
        cxx_sp++;
        g_test_eax();
        int jz_pos = g_jcc(0x84);
        cxx_compile_stmt();
        cxx_e8(0xE9);
        int32_t rel = (int32_t)loop_start - (int32_t)(cxx_cp + 4);
        cxx_e32((uint32_t)rel);
        g_patch(jz_pos);
        return;
    }

    if (cxx_take_kw("cout")) {
        for (;;) {
            cxx_skip_ws();
            if (cxx_sp[0] == '<' && cxx_sp[1] == '<') {
                cxx_sp += 2;
                cxx_skip_ws();
                if (*cxx_sp == '"') {
                    cxx_sp++;
                    int start = cxx_strpool_used;
                    while (*cxx_sp && *cxx_sp != '"') {
                        char c = *cxx_sp++;
                        if (c == '\\' && *cxx_sp) {
                            char e = *cxx_sp++;
                            if      (e=='n')  c='\n';
                            else if (e=='t')  c='\t';
                            else if (e=='\\') c='\\';
                            else if (e=='"')  c='"';
                            else              c=e;
                        }
                        if (cxx_strpool_used < CXX_STRPOOL_SIZE-1)
                            cxx_strpool[cxx_strpool_used++] = c;
                    }
                    if (cxx_strpool_used < CXX_STRPOOL_SIZE-1)
                        cxx_strpool[cxx_strpool_used++] = '\0';
                    if (*cxx_sp == '"') cxx_sp++;
                    g_push_imm((uint32_t)(uintptr_t)&cxx_strpool[start]);
                    g_call((uint32_t)(uintptr_t)&cxx_rt_print_str);
                    g_add_esp_4();
                } else if (cxx_take_kw("endl")) {
                    g_call((uint32_t)(uintptr_t)&cxx_rt_print_nl);
                } else {
                    c_expr();
                    g_push_eax();
                    g_call((uint32_t)(uintptr_t)&cxx_rt_print_int);
                    g_add_esp_4();
                }
            } else break;
        }
        cxx_skip_ws();
        if (*cxx_sp == ';') cxx_sp++;
        else cxx_fail("expected ; after cout");
        return;
    }

    if (cxx_is_alpha(*cxx_sp)) {
        const char* save = cxx_sp;
        char name[32]; int n=0;
        while (cxx_is_alnum(*cxx_sp)) { if (n<31) name[n++]=*cxx_sp; cxx_sp++; }
        name[n]='\0';
        cxx_skip_ws();
        int d = cxx_lookup(name);
        if (d != 0) {
            if (*cxx_sp=='=' && cxx_sp[1] != '=') {
                cxx_sp++; c_expr(); g_mov_local_eax(d);
                cxx_skip_ws();
                if (*cxx_sp==';') cxx_sp++; else cxx_fail("expected ;");
                return;
            }
            if (cxx_sp[0]=='+' && cxx_sp[1]=='+') {
                cxx_sp += 2;
                g_mov_eax_local(d);
                cxx_e8(0x83); cxx_e8(0xC0); cxx_e8(0x01);
                g_mov_local_eax(d);
                cxx_skip_ws();
                if (*cxx_sp==';') cxx_sp++; else cxx_fail("expected ;");
                return;
            }
            if (cxx_sp[0]=='-' && cxx_sp[1]=='-') {
                cxx_sp += 2;
                g_mov_eax_local(d);
                cxx_e8(0x83); cxx_e8(0xE8); cxx_e8(0x01);
                g_mov_local_eax(d);
                cxx_skip_ws();
                if (*cxx_sp==';') cxx_sp++; else cxx_fail("expected ;");
                return;
            }
            if (cxx_sp[0]=='+' && cxx_sp[1]=='=') {
                cxx_sp += 2;
                g_mov_eax_local(d); g_push_eax();
                c_expr(); g_pop_ebx(); g_add_eax_ebx();
                g_mov_local_eax(d);
                cxx_skip_ws();
                if (*cxx_sp==';') cxx_sp++; else cxx_fail("expected ;");
                return;
            }
            if (cxx_sp[0]=='-' && cxx_sp[1]=='=') {
                cxx_sp += 2;
                g_mov_eax_local(d); g_push_eax();
                c_expr(); g_pop_ebx();
                g_xchg_eax_ebx(); g_sub_eax_ebx();
                g_mov_local_eax(d);
                cxx_skip_ws();
                if (*cxx_sp==';') cxx_sp++; else cxx_fail("expected ;");
                return;
            }
            if (cxx_sp[0]=='*' && cxx_sp[1]=='=') {
                cxx_sp += 2;
                g_mov_eax_local(d); g_push_eax();
                c_expr(); g_pop_ebx(); g_imul_eax_ebx();
                g_mov_local_eax(d);
                cxx_skip_ws();
                if (*cxx_sp==';') cxx_sp++; else cxx_fail("expected ;");
                return;
            }
            if (cxx_sp[0]=='/' && cxx_sp[1]=='=') {
                cxx_sp += 2;
                g_mov_eax_local(d); g_push_eax();
                c_expr(); g_pop_ebx();
                g_xchg_eax_ebx(); g_cdq(); g_idiv_ebx();
                g_mov_local_eax(d);
                cxx_skip_ws();
                if (*cxx_sp==';') cxx_sp++; else cxx_fail("expected ;");
                return;
            }
        }
        cxx_sp = save;
    }

    c_expr();
    cxx_skip_ws();
    if (*cxx_sp==';') cxx_sp++;
    else cxx_fail("expected ;");
}

static int cxx_compile_run(const char* src) {
    cxx_cp = 0;
    cxx_ccerr = 0;
    cxx_ccerrmsg = "";
    cxx_nlocals = 0;
    cxx_frame = 0;
    cxx_strpool_used = 0;
    cxx_sp = src;
    cxx_running = 1;

    cxx_skip_ws();
    while (cxx_take_kw("using")) {
        while (*cxx_sp && *cxx_sp != ';') cxx_sp++;
        if (*cxx_sp == ';') cxx_sp++;
        cxx_skip_ws();
    }
    if (!cxx_take_kw("int"))  { cxx_fail("expected 'int main()'"); goto err; }
    cxx_skip_ws();
    if (!cxx_take_kw("main")) { cxx_fail("expected 'main'"); goto err; }
    cxx_skip_ws();
    if (*cxx_sp != '(') { cxx_fail("expected ("); goto err; }
    cxx_sp++; cxx_skip_ws();
    if (*cxx_sp != ')') { cxx_fail("expected )"); goto err; }
    cxx_sp++; cxx_skip_ws();
    if (*cxx_sp != '{') { cxx_fail("expected {"); goto err; }
    cxx_sp++;

    g_push_ebp();
    g_mov_ebp_esp();
    int sub_pos = g_sub_esp_placeholder();

    while (!cxx_ccerr) {
        cxx_skip_ws();
        if (*cxx_sp == '}') { cxx_sp++; break; }
        if (!*cxx_sp) { cxx_fail("unclosed main"); goto err; }
        cxx_compile_stmt();
    }

    g_mov_eax_imm(0);
    g_leave();
    g_ret();

    g_patch_sub_esp(sub_pos, cxx_frame);
    if (cxx_ccerr) goto err;

    {
        typedef int (*fn_t)(void);
        fn_t f = (fn_t)(void*)cxx_code;
        f();
    }
    cxx_running = 0;
    return 1;
err:
    cxx_running = 0;
    return 0;
}

// ================== cat 查看器辅助 ==================
static int cat_count_lines(uint32_t size) {
    if (size == 0) return 0;
    int lines = 0;
    for (uint32_t i = 0; i < size; i++) if (big_buffer[i] == '\n') lines++;
    if (big_buffer[size - 1] != '\n') lines++;
    return lines;
}
static uint32_t cat_line_offset(int line_num) {
    if (line_num <= 0) return 0;
    int line = 0;
    for (uint32_t i = 0; i < cat_file_size; i++) {
        if (line == line_num) return i;
        if (big_buffer[i] == '\n') line++;
    }
    return cat_file_size;
}
static void cat_redraw_viewer(void) {
    clear_screen();
    uint32_t off = cat_line_offset(cat_scroll_line);
    int lines_printed = 0;
    uint32_t i = off;
    while (i < cat_file_size && lines_printed < 23) {
        char c = (char)big_buffer[i++];
        if (c == '\n') { putchar('\n'); lines_printed++; }
        else if (c == '\r') { }
        else if (c == '\t') {
            int next = ((cursor_x / 8) + 1) * 8;
            while (cursor_x < next && cursor_x < 79) putchar(' ');
        } else putchar(c);
    }
    char b[16];
    print("\n");
    print("[");
    print(itoa(cat_scroll_line + 1, b));
    print("/");
    print(itoa(cat_total_lines, b));
    print("] wheel=scroll  click=exit");
}

// ================== PS/2 鼠标 ==================
static int mouse_char_x = 40;
static int mouse_char_y = 15;
static int mouse_old_x = -1;
static int mouse_old_y = -1;
static int mouse_visible = 0;
static char mouse_saved_char = ' ';
static uint8_t mouse_saved_attr = 0x07;
static uint8_t mouse_prev_buttons = 0;
static uint8_t mouse_cycle = 0;
static uint8_t mouse_byte[4];

static void mouse_wait_write(void) {
    uint32_t timeout = 1000000;
    while (timeout--) if (!(inb(0x64) & 0x02)) return;
}
static void mouse_write_cmd(uint8_t val) {
    mouse_wait_write();
    outb(0x64, 0xD4);
    mouse_wait_write();
    outb(0x60, val);
}
static uint8_t mouse_read_byte(void) {
    uint32_t timeout = 1000000;
    while (timeout--) {
        uint8_t st = inb(0x64);
        if ((st & 0x01) && (st & 0x20)) return inb(0x60);
        if ((st & 0x01) && !(st & 0x20)) inb(0x60);
    }
    return 0xFF;
}
static int mouse_wait_ack(void) {
    for (int i = 0; i < 32; i++) {
        uint8_t b = mouse_read_byte();
        if (b == 0xFA) return 1;
        if (b == 0xFF) return 0;
    }
    return 0;
}
static void mouse_draw(void) {
    if (!mouse_enabled) return;
    char* video = (char*)0xB8000;
    if (mouse_visible && mouse_old_x >= 0 && mouse_old_y >= 0) {
        int pos = mouse_old_y * 80 + mouse_old_x;
        video[2*pos] = mouse_saved_char;
        video[2*pos+1] = mouse_saved_attr;
    }
    int pos = mouse_char_y * 80 + mouse_char_x;
    mouse_saved_char = video[2*pos];
    mouse_saved_attr = video[2*pos+1];
    uint8_t attr = (mouse_prev_buttons & 0x01) ? 0x4F : 0x70;
    video[2*pos] = ' ';
    video[2*pos+1] = attr;
    mouse_visible = 1;
    mouse_old_x = mouse_char_x;
    mouse_old_y = mouse_char_y;
}
static void mouse_click_char(void) {
    char c = mouse_saved_char;
    if (c < 0x20 || c > 0x7E) return;
    if (cmd_index >= CMD_BUFFER_SIZE - 1) return;
    cmd_buffer[cmd_index++] = c;
    cmd_buffer[cmd_index] = '\0';
    putchar(c);
    mouse_visible = 0;
    mouse_draw();
}

void mouse_handler_c(void) {
    if (!mouse_enabled) {
        uint8_t st = inb(0x64);
        if ((st & 0x01) && (st & 0x20)) inb(0x60);
        outb(0xA0, 0x20); outb(0x20, 0x20);
        return;
    }
    uint8_t st = inb(0x64);
    if (!(st & 0x01) || !(st & 0x20)) {
        outb(0xA0, 0x20); outb(0x20, 0x20);
        return;
    }
    uint8_t data = inb(0x60);

    if (mouse_wheel_mode) {
        switch (mouse_cycle) {
            case 0: if (data & 0x08) { mouse_byte[0] = data; mouse_cycle = 1; } break;
            case 1: mouse_byte[1] = data; mouse_cycle = 2; break;
            case 2: mouse_byte[2] = data; mouse_cycle = 3; break;
            case 3: {
                uint8_t z = data;
                mouse_cycle = 0;
                uint8_t buttons = mouse_byte[0];
                int dx = (int)(signed char)mouse_byte[1];
                int dy = (int)(signed char)mouse_byte[2];
                int wheel = z & 0x0F;
                if (wheel >= 8) wheel -= 16;
                mouse_px += dx; mouse_py -= dy;
                if (mouse_px < 0) mouse_px = 0;
                if (mouse_px > 639) mouse_px = 639;
                if (mouse_py < 0) mouse_py = 0;
                if (mouse_py > 479) mouse_py = 479;
                int cx = mouse_px / 8, cy = mouse_py / 16;
                int moved = (cx != mouse_char_x || cy != mouse_char_y);
                int buttons_changed = (buttons != mouse_prev_buttons);
                int left_down = (buttons & 0x01) && !(mouse_prev_buttons & 0x01);
                mouse_prev_buttons = buttons;
                if (moved || buttons_changed) {
                    mouse_char_x = cx; mouse_char_y = cy;
                    mouse_draw();
                }
                if (left_down) {
                    if (cat_viewing) cat_viewing = 0;
                    else mouse_click_char();
                }
                if (wheel != 0 && cat_viewing) {
                    int new_line = cat_scroll_line - wheel;
                    if (new_line < 0) new_line = 0;
                    if (new_line > cat_total_lines - 1) new_line = cat_total_lines - 1;
                    if (new_line != cat_scroll_line) { cat_scroll_line = new_line; cat_dirty = 1; }
                }
            } break;
        }
    } else {
        switch (mouse_cycle) {
            case 0: if (data & 0x08) { mouse_byte[0] = data; mouse_cycle = 1; } break;
            case 1: mouse_byte[1] = data; mouse_cycle = 2; break;
            case 2: {
                mouse_byte[2] = data;
                mouse_cycle = 0;
                uint8_t buttons = mouse_byte[0];
                int dx = (int)(signed char)mouse_byte[1];
                int dy = (int)(signed char)mouse_byte[2];
                mouse_px += dx; mouse_py -= dy;
                if (mouse_px < 0) mouse_px = 0;
                if (mouse_px > 639) mouse_px = 639;
                if (mouse_py < 0) mouse_py = 0;
                if (mouse_py > 479) mouse_py = 479;
                int cx = mouse_px / 8, cy = mouse_py / 16;
                int moved = (cx != mouse_char_x || cy != mouse_char_y);
                int buttons_changed = (buttons != mouse_prev_buttons);
                int left_down = (buttons & 0x01) && !(mouse_prev_buttons & 0x01);
                mouse_prev_buttons = buttons;
                if (moved || buttons_changed) {
                    mouse_char_x = cx; mouse_char_y = cy;
                    mouse_draw();
                }
                if (left_down) {
                    if (cat_viewing) cat_viewing = 0;
                    else mouse_click_char();
                }
            } break;
        }
    }
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

static void mouse_erase(void) {
    if (mouse_visible && mouse_old_x >= 0 && mouse_old_y >= 0) {
        char* video = (char*)0xB8000;
        int pos = mouse_old_y * 80 + mouse_old_x;
        video[2*pos]   = mouse_saved_char;
        video[2*pos+1] = mouse_saved_attr;
    }
    mouse_visible = 0;
    mouse_old_x = -1;
    mouse_old_y = -1;
}
void mouse_disable(void) {
    if (!mouse_enabled) return;
    mouse_enabled = 0;
    cat_viewing = 0;
    mouse_write_cmd(0xF5);
    mouse_read_byte();
    uint8_t mask = inb(0xA1);
    outb(0xA1, mask | 0x10);
    mouse_erase();
    mouse_cycle = 0;
    mouse_prev_buttons = 0;
}
void mouse_enable(void) {
    if (mouse_enabled) return;
    uint8_t mask = inb(0xA1);
    outb(0xA1, mask & ~0x10);
    mouse_write_cmd(0xF4);
    mouse_read_byte();
    mouse_enabled = 1;
    mouse_cycle = 0;
    mouse_draw();
}
int mouse_is_enabled(void) { return mouse_enabled; }

void mouse_init(void) {
    kbc_flush_output();
    mouse_wait_write();
    outb(0x64, 0xA8);
    mouse_wait_write();
    outb(0x64, 0x20);
    uint8_t status = mouse_read_byte();
    if (status == 0xFF) status = 0x47;
    status |= 0x03;
    status &= ~0x30;
    mouse_wait_write();
    outb(0x64, 0x60);
    mouse_wait_write();
    outb(0x60, status);
    kbc_flush_output();
    mouse_write_cmd(0xF6);
    mouse_wait_ack();
    mouse_wheel_mode = 0;
    {
        int ok = 1;
        mouse_write_cmd(0xF3); if (!mouse_wait_ack()) ok = 0;
        mouse_write_cmd(200);   if (!mouse_wait_ack()) ok = 0;
        if (ok) {
            mouse_write_cmd(0xF3); if (!mouse_wait_ack()) ok = 0;
            mouse_write_cmd(100);  if (!mouse_wait_ack()) ok = 0;
        }
        if (ok) {
            mouse_write_cmd(0xF3); if (!mouse_wait_ack()) ok = 0;
            mouse_write_cmd(80);   if (!mouse_wait_ack()) ok = 0;
        }
        if (ok) {
            mouse_write_cmd(0xF2);
            if (mouse_wait_ack()) {
                uint8_t dev_id = mouse_read_byte();
                if (dev_id == 3 || dev_id == 4) mouse_wheel_mode = 1;
            }
        }
    }
    kbc_flush_output();
    mouse_write_cmd(0xF4);
    mouse_wait_ack();
    mouse_cycle = 0;
    mouse_draw();
}

// ================== 行输入（write 模式） ==================
static int read_line_blocking(char* out, int max) {
    line_ready = 0;
    line_buffer[0] = '\0';
    cmd_index = 0;
    cmd_buffer[0] = '\0';
    line_reading = 1;
    while (!line_ready) __asm__ volatile("sti; hlt");
    line_reading = 0;
    strcpy_safe(out, line_buffer, max);
    return 1;
}
#define BOOT_STAGE2_LBA  1
#define BOOT_INFO_LBA    33
#define BOOT_KERNEL_LBA  128
static int install_to_disk(void) {
    uint32_t kernel_size    = (uint32_t)((uintptr_t)_kernel_file_end -
                                         (uintptr_t)_kernel_start);
    uint32_t kernel_sectors = (kernel_size + 511) / 512;
    uint32_t bss_start      = (uint32_t)(uintptr_t)_bss_start;
    uint32_t bss_size       = (uint32_t)((uintptr_t)_bss_end -
                                         (uintptr_t)_bss_start);

    char b[16];
    print("Kernel image: ");
    print(itoa((int)kernel_size, b));
    print(" bytes / ");
    print(itoa((int)kernel_sectors, b));
    print(" sectors\n");
    print("BSS: ");
    print(itoa((int)bss_start, b));
    print(" size=");
    print(itoa((int)bss_size, b));
    print("\n");

    if (kernel_sectors > (FAT32_VOLUME_OFFSET - BOOT_KERNEL_LBA)) {
        print("Error: kernel image too large for boot area.\n");
        return 0;
    }
    if (mbr_bin_len > 512 || stage2_bin_len > 32 * 512) {
        print("Error: boot binaries invalid.\n");
        return 0;
    }

    ata_flush_suppress = 1;
    uint8_t sec[ATA_SECTOR_SIZE];

    // 1. MBR
    for (int i = 0; i < ATA_SECTOR_SIZE; i++)
        sec[i] = ((unsigned)i < mbr_bin_len) ? mbr_bin[i] : 0;
    if (!ata_write_sector(0, sec)) goto fail;
    print("MBR written.\n");

    // 2. Stage2（32 个扇区）
    for (int s = 0; s < 32; s++) {
        uint32_t off = (uint32_t)s * 512;
        for (int i = 0; i < ATA_SECTOR_SIZE; i++) {
            uint32_t k = off + i;
            sec[i] = (k < stage2_bin_len) ? stage2_bin[k] : 0;
        }
        if (!ata_write_sector(BOOT_STAGE2_LBA + s, sec)) goto fail;
    }
    print("Stage2 written.\n");

    // 3. 引导信息
    for (int i = 0; i < ATA_SECTOR_SIZE; i++) sec[i] = 0;
    *(uint32_t*)(sec + 0)  = 0x4E4F5641;
    *(uint32_t*)(sec + 4)  = BOOT_KERNEL_LBA;
    *(uint32_t*)(sec + 8)  = kernel_sectors;
    *(uint32_t*)(sec + 12) = 0x100000;
    *(uint32_t*)(sec + 16) = (uint32_t)(uintptr_t)_kernel_entry;
    *(uint32_t*)(sec + 20) = bss_start;
    *(uint32_t*)(sec + 24) = bss_size;
    if (!ata_write_sector(BOOT_INFO_LBA, sec)) goto fail;
    print("Boot info written.\n");

    // 4. 内核镜像
    print("Kernel: ");
    for (uint32_t s = 0; s < kernel_sectors; s++) {
        uint32_t off = s * 512;
        for (int i = 0; i < ATA_SECTOR_SIZE; i++) {
            uint32_t k = off + i;
            sec[i] = (k < kernel_size) ? _kernel_start[k] : 0;
        }
        if (!ata_write_sector(BOOT_KERNEL_LBA + s, sec)) goto fail;
        if ((s & 0x3F) == 0) putchar('.');
    }
    print(" done\n");

    ata_flush_suppress = 0;
    ata_flush();
    print("Install complete. Reboot from this disk.\n");
    return 1;

fail:
    ata_flush_suppress = 0;
    ata_flush();
    print("Install FAILED.\n");
    return 0;
}
// ================== 处理命令 ==================
static void process_command(void) {
    if (strcmp(cmd_buffer, "ls") == 1) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        uint32_t cluster = fat32_cur_cluster;
        uint8_t buf[ATA_SECTOR_SIZE];
        int count = 0, stop = 0;
        while (cluster >= 2 && cluster < FAT32_EOC && !stop) {
            uint32_t base = fat32_cluster_sector(cluster);
            for (uint32_t s = 0; s < fat32_spc && !stop; s++) {
                if (!ata_read_sector(base + s, buf)) { stop = 1; break; }
                struct fat32_dir_entry* entries = (struct fat32_dir_entry*)buf;
                for (int i = 0; i < 16; i++) {
                    if (entries[i].name[0] == 0x00) { stop = 1; break; }
                    if (entries[i].name[0] == 0xE5) continue;
                    if (entries[i].attributes == ATTR_LFN) continue;
                    if (entries[i].attributes & ATTR_VOLUME_ID) continue;
                    if (entries[i].name[0] == '.') {
                        if (entries[i].name[1] == ' ' || entries[i].name[1] == '.') continue;
                    }
                    char fname[16];
                    format_name_83(&entries[i], fname);
                    if (entries[i].attributes & ATTR_DIRECTORY) print("[DIR]  ");
                    else print("[FILE] ");
                    print(fname);
                    print("  ");
                    char sz[16];
                    print(itoa((int)entries[i].file_size, sz));
                    print(" bytes\n");
                    count++;
                }
            }
            if (!stop) cluster = fat32_get_fat(cluster);
        }
        if (count == 0) print("(empty)\n");
    }
    else if (starts_with(cmd_buffer, "mkdir ")) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* dname = cmd_buffer + 6;
        while (*dname == ' ') dname++;
        if (*dname == '\0') { print("Error: missing name\n"); goto done; }
        struct fat32_dir_entry tmp;
        if (fat32_find_in_dir(fat32_cur_cluster, dname, &tmp, 0, 0) == 0) {
            print("Error: already exists\n"); goto done;
        }
        uint32_t new_c = fat32_alloc_cluster();
        if (new_c == 0) { print("Error: disk full\n"); goto done; }
        uint8_t dirbuf[ATA_SECTOR_SIZE];
        for (int i = 0; i < ATA_SECTOR_SIZE; i++) dirbuf[i] = 0;
        struct fat32_dir_entry* e = (struct fat32_dir_entry*)dirbuf;
        for (int i = 0; i < 11; i++) e[0].name[i] = ' ';
        e[0].name[0] = '.'; e[0].attributes = ATTR_DIRECTORY;
        dir_set_cluster(&e[0], new_c);
        for (int i = 0; i < 11; i++) e[1].name[i] = ' ';
        e[1].name[0] = '.'; e[1].name[1] = '.'; e[1].attributes = ATTR_DIRECTORY;
        dir_set_cluster(&e[1], fat32_cur_cluster);
        uint32_t base = fat32_cluster_sector(new_c);
        if (!ata_write_sector(base, dirbuf)) { print("Error: write failed\n"); goto done; }
        for (int i = 0; i < ATA_SECTOR_SIZE; i++) dirbuf[i] = 0;
        for (uint32_t s = 1; s < fat32_spc; s++) ata_write_sector(base + s, dirbuf);
        if (fat32_create_in_dir(fat32_cur_cluster, dname, ATTR_DIRECTORY, new_c, 0) != 0) {
            fat32_free_chain(new_c);
            print("Error: cannot create\n"); goto done;
        }
        print("Directory created.\n");
    }
    else if (starts_with(cmd_buffer, "cd ")) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* path = cmd_buffer + 3;
        while (*path == ' ') path++;
        if (*path == '\0' || strcmp(path, "/") == 1) {
            fat32_cur_cluster = fat32_root_cluster;
            strcpy_safe(fat32_cur_path, "/", 256);
            print("Changed to /\n"); goto done;
        }
        if (strcmp(path, "..") == 1) {
            if (fat32_cur_cluster == fat32_root_cluster) { print("Already at root.\n"); goto done; }
            uint8_t buf[ATA_SECTOR_SIZE];
            if (!ata_read_sector(fat32_cluster_sector(fat32_cur_cluster), buf)) { print("Error: read failed\n"); goto done; }
            struct fat32_dir_entry* entries = (struct fat32_dir_entry*)buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == '.' && entries[i].name[1] == '.') {
                    uint32_t p = dir_cluster(&entries[i]);
                    if (p < 2) p = fat32_root_cluster;
                    fat32_cur_cluster = p;
                    print("Changed to ..\n"); goto done;
                }
                if (entries[i].name[0] == 0x00) break;
            }
            print("Error: no parent\n"); goto done;
        }
        struct fat32_dir_entry entry;
        if (fat32_find_in_dir(fat32_cur_cluster, path, &entry, 0, 0) != 0) { print("Directory not found.\n"); goto done; }
        if (!(entry.attributes & ATTR_DIRECTORY)) { print("Not a directory.\n"); goto done; }
        uint32_t c = dir_cluster(&entry);
        if (c < 2) c = fat32_root_cluster;
        fat32_cur_cluster = c;
        print("Changed to: "); print(path); print("\n");
    }
    else if (strcmp(cmd_buffer, "pwd") == 1) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        if (fat32_cur_cluster == fat32_root_cluster) print("/\n");
        else { print("/"); print(fat32_cur_path); print("\n"); }
    }
    else if (starts_with(cmd_buffer, "touch ")) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* fname = cmd_buffer + 6;
        while (*fname == ' ') fname++;
        if (*fname == '\0') { print("Error: missing name\n"); goto done; }
        struct fat32_dir_entry tmp;
        if (fat32_find_in_dir(fat32_cur_cluster, fname, &tmp, 0, 0) == 0) { print("File already exists.\n"); goto done; }
        if (fat32_create_in_dir(fat32_cur_cluster, fname, ATTR_ARCHIVE, 0, 0) != 0) { print("Error: cannot create\n"); goto done; }
        print("File created.\n");
    }
    else if (starts_with(cmd_buffer, "cat ")) {
        mouse_enable();
        clear_screen();
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* fname = cmd_buffer + 4;
        while (*fname == ' ') fname++;
        struct fat32_dir_entry entry;
        if (fat32_find_in_dir(fat32_cur_cluster, fname, &entry, 0, 0) != 0) { print("File not found.\n"); goto done; }
        if (entry.attributes & ATTR_DIRECTORY) { print("Is a directory.\n"); goto done; }
        if (entry.file_size == 0) {
            print("(empty)\n");
            print("\n--- Left-click to exit ---");
            mouse_prev_buttons = 0; mouse_cycle = 0;
            cat_viewing = 1;
            __asm__ volatile("sti");
            while (cat_viewing) __asm__ volatile("hlt");
            clear_screen();
            cmd_index = 0; cmd_buffer[0] = '\0';
            print("> "); history_pos = 0;
            mouse_disable();
            return;
        }
        uint32_t max = BIG_BUF_SIZE;
        cat_file_size = fat32_read_file(&entry, big_buffer, max);
        big_buffer[cat_file_size] = '\0';
        cat_total_lines = cat_count_lines(cat_file_size);
        cat_scroll_line = 0;
        cat_redraw_viewer();
        mouse_prev_buttons = 0; mouse_cycle = 0;
        cat_dirty = 0;
        cat_viewing = 1;
        __asm__ volatile("sti");
        while (cat_viewing) {
            if (cat_dirty) { cat_dirty = 0; cat_redraw_viewer(); }
            __asm__ volatile("hlt");
        }
        clear_screen();
        cmd_index = 0; cmd_buffer[0] = '\0';
        print("> "); history_pos = 0;
        mouse_disable();
        return;
    }
    else if (strcmp(cmd_buffer, "install") == 1) {
        if (!allow_install) {
            print("Error: OS is already installed on disk. 'install' is disabled.\n");
            goto done;
        }
        if (!fat32_mounted) {
            print("Warning: FAT32 not mounted, but proceeding anyway.\n");
        }
        print("Installing Nova OS to disk...\n");
        if (!install_to_disk()) {
            print("Installation aborted.\n");
        }
    }
    else if (starts_with(cmd_buffer, "write ")) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* fname = cmd_buffer + 6;
        while (*fname == ' ') fname++;
        if (*fname == '\0') { print("Error: missing name\n"); goto done; }
        struct fat32_dir_entry entry;
        uint32_t sector; int idx;
        if (fat32_find_in_dir(fat32_cur_cluster, fname, &entry, &sector, &idx) != 0) {
            if (fat32_create_in_dir(fat32_cur_cluster, fname, ATTR_ARCHIVE, 0, 0) != 0) { print("Error: cannot create\n"); goto done; }
            if (fat32_find_in_dir(fat32_cur_cluster, fname, &entry, &sector, &idx) != 0) { print("Error: cannot open\n"); goto done; }
        }
        if (entry.attributes & ATTR_DIRECTORY) { print("Is a directory.\n"); goto done; }
        print("Enter content ('.' on a line to finish, max 5MB):\n");
        uint32_t total = 0;
        int aborted = 0;
        while (1) {
            char line[CMD_BUFFER_SIZE];
            print("| ");
            if (!read_line_blocking(line, CMD_BUFFER_SIZE)) break;
            if (line[0] == '.' && line[1] == '\0') break;
            int ll = 0; while (line[ll]) ll++;
            if (total + (uint32_t)ll + 1 > BIG_BUF_SIZE) { print("Error: content exceeds 5MB limit\n"); aborted = 1; break; }
            for (int k = 0; k < ll; k++) big_buffer[total++] = line[k];
            big_buffer[total++] = '\n';
        }
        line_reading = 0;
        if (!aborted) {
            if (fat32_overwrite_file(sector, idx, big_buffer, total) != 0) { print("Error: write failed\n"); goto done; }
            char b[16];
            print("Written ");
            print(itoa((int)total, b));
            print(" bytes.\n");
        }
    }
    else if (starts_with(cmd_buffer, "rm ")) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* fname = cmd_buffer + 3;
        while (*fname == ' ') fname++;
        struct fat32_dir_entry entry;
        uint32_t sector; int idx;
        if (fat32_find_in_dir(fat32_cur_cluster, fname, &entry, &sector, &idx) != 0) { print("Not found.\n"); goto done; }
        if (entry.attributes & ATTR_DIRECTORY) {
            uint32_t c = dir_cluster(&entry);
            int non_empty = 0;
            uint8_t buf[ATA_SECTOR_SIZE];
            uint32_t cc = c;
            int stop = 0;
            while (cc >= 2 && cc < FAT32_EOC && !stop) {
                uint32_t base = fat32_cluster_sector(cc);
                for (uint32_t s = 0; s < fat32_spc && !stop; s++) {
                    if (!ata_read_sector(base + s, buf)) { stop = 1; break; }
                    struct fat32_dir_entry* es = (struct fat32_dir_entry*)buf;
                    for (int k = 0; k < 16; k++) {
                        if (es[k].name[0] == 0x00) { stop = 1; break; }
                        if (es[k].name[0] == 0xE5) continue;
                        if (es[k].attributes == ATTR_LFN) continue;
                        if (es[k].name[0] == '.') continue;
                        non_empty = 1; stop = 1; break;
                    }
                }
                if (!stop) cc = fat32_get_fat(cc);
            }
            if (non_empty) { print("Error: directory not empty\n"); goto done; }
            fat32_free_chain(c);
        } else {
            uint32_t c = dir_cluster(&entry);
            if (c >= 2 && c < FAT32_EOC) fat32_free_chain(c);
        }
        uint8_t dbuf[ATA_SECTOR_SIZE];
        if (!ata_read_sector(sector, dbuf)) { print("Error: read failed\n"); goto done; }
        ((struct fat32_dir_entry*)dbuf)[idx].name[0] = 0xE5;
        ata_write_sector(sector, dbuf);
        print("Deleted.\n");
    }
    // -------- run <file.bat> --------
    else if (starts_with(cmd_buffer, "run ")) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* fname = cmd_buffer + 4;
        while (*fname == ' ') fname++;
        if (*fname == '\0') { print("Error: missing filename\n"); goto done; }
        char namebuf[64];
        int n = 0;
        while (fname[n] && n < 63) { namebuf[n] = fname[n]; n++; }
        namebuf[n] = '\0';
        for (int i = 0; i < n; i++)
            if (namebuf[i] >= 'a' && namebuf[i] <= 'z')
                namebuf[i] = (char)(namebuf[i] - 'a' + 'A');
        if (n < 4 || namebuf[n-4] != '.' || namebuf[n-3] != 'B' || namebuf[n-2] != 'A' || namebuf[n-1] != 'T') {
            print("Error: only .bat files can be run\n");
            goto done;
        }
        struct fat32_dir_entry entry;
        if (fat32_find_in_dir(fat32_cur_cluster, namebuf, &entry, 0, 0) != 0) { print("File not found.\n"); goto done; }
        if (entry.attributes & ATTR_DIRECTORY) { print("Is a directory.\n"); goto done; }
        if (entry.file_size == 0) { print("Empty batch file.\n"); goto done; }
        if (entry.file_size > BIG_BUF_SIZE - 1) { print("Batch file too large.\n"); goto done; }
        if (batch_depth >= BATCH_MAX_DEPTH) { print("Error: batch nesting too deep.\n"); goto done; }
        uint32_t got = fat32_read_file(&entry, big_buffer, BIG_BUF_SIZE - 1);
        if (got != entry.file_size) { print("Read failed.\n"); goto done; }
        big_buffer[got] = '\0';
        batch_depth++;
        int saved_mode = batch_mode;
        batch_mode = 1;
        char linebuf[CMD_BUFFER_SIZE];
        uint32_t pos = 0;
        while (pos < got) {
            int li = 0;
            while (pos < got && big_buffer[pos] != '\n' && li < CMD_BUFFER_SIZE - 1) {
                char c = (char)big_buffer[pos++];
                if (c == '\r') continue;
                linebuf[li++] = c;
            }
            if (pos < got && big_buffer[pos] == '\n') pos++;
            linebuf[li] = '\0';
            int s = 0;
            while (s < li && (linebuf[s] == ' ' || linebuf[s] == '\t')) s++;
            int e = li;
            while (e > s && (linebuf[e-1] == ' ' || linebuf[e-1] == '\t')) e--;
            int len = e - s;
            if (len == 0) continue;
            if (linebuf[s] == '#' || linebuf[s] == ';') continue;
            for (int k = 0; k < len; k++) cmd_buffer[k] = linebuf[s + k];
            cmd_buffer[len] = '\0';
            cmd_index = len;
            putchar('>'); putchar(' ');
            for (int k = 0; k < len; k++) putchar(linebuf[s + k]);
            putchar('\n');
            process_command();
        }
        batch_mode = saved_mode;
        batch_depth--;
        if (!batch_mode) print("Batch finished.\n");
        goto done;
    }
    // -------- cxx <file.cpp> : 编译并执行 C++ --------
    else if (starts_with(cmd_buffer, "cxx ")) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* fname = cmd_buffer + 4;
        while (*fname == ' ') fname++;
        if (*fname == '\0') { print("Usage: cxx <file.cpp>\n"); goto done; }
        struct fat32_dir_entry entry;
        if (fat32_find_in_dir(fat32_cur_cluster, fname, &entry, 0, 0) != 0) { print("File not found.\n"); goto done; }
        if (entry.attributes & ATTR_DIRECTORY) { print("Is a directory.\n"); goto done; }
        if (entry.file_size == 0) { print("(empty file)\n"); goto done; }
        if (entry.file_size > BIG_BUF_SIZE - 1) { print("Source too large.\n"); goto done; }
        uint32_t got = fat32_read_file(&entry, big_buffer, BIG_BUF_SIZE - 1);
        big_buffer[got] = '\0';
        print("--- compile & run ---\n");
        if (cxx_compile_run((const char*)big_buffer)) {
            print("\n--- exit 0 ---\n");
        } else {
            print("\n--- compile error: ");
            print(cxx_ccerrmsg);
            print(" ---\n");
        }
    }
    else if (starts_with(cmd_buffer, "format")) {
        uint32_t disk = ata_disk_sectors();
        if (disk == 0) { print("No disk detected.\n"); goto done; }
        if (disk <= FAT32_VOLUME_OFFSET + 0x10000) { print("Disk too small.\n"); goto done; }
        uint32_t vol = disk - FAT32_VOLUME_OFFSET;
        if (vol > 0x1000000) vol = 0x1000000;
        fat32_format(vol);
        print("Format complete.\n");
    }
    else if (strcmp(cmd_buffer, "cls") == 1) clear_screen();
    else if (strcmp(cmd_buffer, "help") == 1) {
        print("install                 - Write Nova OS bootloader + kernel to disk\n");
        print("cls                     - Clear screen\n");
        print("help                    - This help\n");
        print("reboot                  - Reboot\n");
        print("version                 - Version info\n");
        print("echo <text>             - Echo text\n");
        print("beep                    - Beep\n");
        print("shutdown                - Halt\n");
        print("calc <a> <op> <b>       - Calculator\n");
        print("ls                      - List files\n");
        print("mkdir <name>            - Create directory\n");
        print("cd <path> | cd ..       - Change directory\n");
        print("pwd                     - Print working dir\n");
        print("touch <name>            - Create empty file\n");
        print("cat <name>              - Show file (wheel to scroll, click to exit)\n");
        print("write <name>            - Write content (multi-line, '.' to end, max 5MB)\n");
        print("rm <name>               - Delete file/empty dir\n");
        print("format                  - Reformat as FAT32\n");
        print("mouse on | mouse off    - Enable/disable mouse\n");
        print("run <file.bat>          - Execute a batch script\n");
        print("cxx <file.cpp>          - Compile C++ to x86 machine code and run\n");
        print("Tip: up/down for history\n");
    }
    else if (strcmp(cmd_buffer, "mouse off") == 1) {
        if (!mouse_is_enabled()) print("Mouse already disabled.\n");
        else { mouse_disable(); print("Mouse disabled.\n"); }
    }
    else if (strcmp(cmd_buffer, "mouse on") == 1) {
        if (mouse_is_enabled()) print("Mouse already enabled.\n");
        else { mouse_enable(); print("Mouse enabled.\n"); }
    }
    else if (strcmp(cmd_buffer, "mouse") == 1) {
        print("Mouse is ");
        print(mouse_is_enabled() ? "ON" : "OFF");
        print("\nUsage: mouse on | mouse off\n");
    }
    else if (strcmp(cmd_buffer, "reboot") == 1) {
        print("Rebooting...\n");
        __asm__ volatile("cli");
        uint8_t rcr = inb(0xCF9);
        outb(0xCF9, (rcr & 0xF9) | 0x02);
        outb(0xCF9, (rcr & 0xF9) | 0x06);
        for (volatile int i = 0; i < 10000000; i++) { }
        for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 0x02)) break;
        outb(0x64, 0xFE);
        for (volatile int i = 0; i < 10000000; i++) { }
        struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = {0, 0};
        __asm__ volatile("lidt (%0)" : : "r"(&null_idt));
        __asm__ volatile("int $0x03");
        while (1) __asm__ volatile("pause");
    }
    else if (strcmp(cmd_buffer, "version") == 1) {
        print("Nova OS Kernel v0.8\n");
    }
    else if (starts_with(cmd_buffer, "echo ")) {
        print(cmd_buffer + 5); print("\n");
    }
    else if (strcmp(cmd_buffer, "beep") == 1) {
        __asm__ volatile(
            "mov $0xB6, %%al\n out %%al, $0x43\n"
            "mov $0x57, %%al\n out %%al, $0x42\n"
            "mov $0x04, %%al\n out %%al, $0x42\n"
            "in $0x61, %%al\n or $0x03, %%al\n out %%al, $0x61\n"
            "mov $0x10, %%cx\n .loop:\n loop .loop\n"
            "in $0x61, %%al\n and $0xFC, %%al\n out %%al, $0x61\n"
            : : : "eax", "ecx", "dx"
        );
        print("Beep OK\n");
    }
    else if (strcmp(cmd_buffer, "shutdown") == 1) {
        print("System halted. You may power off the VM.\n");
        while (1) __asm__ volatile("hlt");
    }
    else if (starts_with(cmd_buffer, "calc ")) {
        const char* expr = cmd_buffer + 5;
        while (*expr == ' ') expr++;
        char num1_str[16]; int i = 0;
        while (*expr != ' ' && *expr != '\0' && i < 15) num1_str[i++] = *expr++;
        num1_str[i] = '\0';
        int num1 = atoi(num1_str);
        while (*expr == ' ') expr++;
        if (*expr == '\0') { print("Error: missing operator\n"); goto calc_done; }
        char op = *expr++;
        while (*expr == ' ') expr++;
        i = 0;
        char num2_str[16];
        while (*expr != '\0' && *expr != ' ' && i < 15) num2_str[i++] = *expr++;
        num2_str[i] = '\0';
        int num2 = atoi(num2_str);
        int result = 0, valid = 1;
        switch (op) {
            case '+': result = num1 + num2; break;
            case '-': result = num1 - num2; break;
            case '*': result = num1 * num2; break;
            case '/':
                if (num2 == 0) { print("Error: div by zero\n"); valid = 0; }
                else result = num1 / num2;
                break;
            default: print("Error: bad operator\n"); valid = 0; break;
        }
        if (valid) { char b[32]; print(itoa(result, b)); print("\n"); }
    calc_done: ;
    }
    else if (cmd_buffer[0] != 0) {
        print("Unknown command: ");
        print(cmd_buffer);
        print("\n");
    }

done:
    if (batch_mode) {
        cmd_index = 0;
        cmd_buffer[0] = '\0';
        return;
    }
    if (cmd_buffer[0] != '\0') {
        strcpy_safe(history[history_tail], cmd_buffer, CMD_BUFFER_SIZE);
        history_tail = (history_tail + 1) % HISTORY_SIZE;
        if (history_count < HISTORY_SIZE) history_count++;
    }
    history_pos = 0;
    cmd_index = 0;
    cmd_buffer[0] = '\0';
    print("> ");
}

// ================== 扫描码映射 ==================
char scancode_to_ascii(unsigned char scancode) {
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
    if (shift_pressed) {
        switch (scancode) {
            case 0x02: c = '!'; break;  case 0x03: c = '@'; break;
            case 0x04: c = '#'; break;  case 0x05: c = '$'; break;
            case 0x06: c = '%'; break;  case 0x07: c = '^'; break;
            case 0x08: c = '&'; break;  case 0x09: c = '*'; break;
            case 0x0A: c = '('; break;  case 0x0B: c = ')'; break;
            case 0x0C: c = '_'; break;  case 0x0D: c = '+'; break;
            case 0x10: c = 'Q'; break;  case 0x11: c = 'W'; break;
            case 0x12: c = 'E'; break;  case 0x13: c = 'R'; break;
            case 0x14: c = 'T'; break;  case 0x15: c = 'Y'; break;
            case 0x16: c = 'U'; break;  case 0x17: c = 'I'; break;
            case 0x18: c = 'O'; break;  case 0x19: c = 'P'; break;
            case 0x1A: c = '{'; break;  case 0x1B: c = '}'; break;
            case 0x1E: c = 'A'; break;  case 0x1F: c = 'S'; break;
            case 0x20: c = 'D'; break;  case 0x21: c = 'F'; break;
            case 0x22: c = 'G'; break;  case 0x23: c = 'H'; break;
            case 0x24: c = 'J'; break;  case 0x25: c = 'K'; break;
            case 0x26: c = 'L'; break;  case 0x27: c = ':'; break;
            case 0x28: c = '"'; break;  case 0x29: c = '~'; break;
            case 0x2B: c = '|'; break;  case 0x2C: c = 'Z'; break;
            case 0x2D: c = 'X'; break;  case 0x2E: c = 'C'; break;
            case 0x2F: c = 'V'; break;  case 0x30: c = 'B'; break;
            case 0x31: c = 'N'; break;  case 0x32: c = 'M'; break;
            case 0x33: c = '<'; break;  case 0x34: c = '>'; break;
            case 0x35: c = '?'; break;
        }
    }
    return c;
}

// ================== GDT / IDT / PIC ==================
struct gdt_entry {
    uint16_t limit_low; uint16_t base_low;
    uint8_t  base_middle; uint8_t access;
    uint8_t  granularity; uint8_t base_high;
} __attribute__((packed));
struct gdt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));

struct gdt_entry gdt[3];
struct gdt_ptr gp;

void setup_gdt(void) {
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

struct idt_entry {
    uint16_t base_low; uint16_t sel;
    uint8_t  always0; uint8_t flags;
    uint16_t base_high;
} __attribute__((packed));
struct idt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr ip;

extern void keyboard_handler_wrapper();

void keyboard_handler_c(void) {
    interrupt_counter++;

    /* C++ 编译/运行期间，丢弃全部键盘输入 */
    if (cxx_running) {
        uint8_t st = inb(0x64);
        if ((st & 0x01) && !(st & 0x20)) inb(0x60);
        outb(0x20, 0x20);
        return;
    }

    uint8_t st = inb(0x64);
    if (!(st & 0x01) || (st & 0x20)) {
        __asm__ volatile("mov $0x20, %%al\n out %%al, $0x20" : : : "eax");
        disable_hardware_cursor();
        return;
    }

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

    if (scancode == 0x48) {
        if (history_count > 0 && !line_reading) {
            if (history_pos == 0) {
                history_pos = (history_count < HISTORY_SIZE) ?
                              history_count - 1 :
                              (history_tail == 0 ? HISTORY_SIZE - 1 : history_tail - 1);
            } else {
                int prev = (history_pos - 1 + HISTORY_SIZE) % HISTORY_SIZE;
                history_pos = prev;
            }
            while (cmd_index > 0) { backspace(); cmd_index--; }
            char* cmd = history[history_pos];
            int i = 0;
            while (cmd[i] && i < CMD_BUFFER_SIZE - 1) {
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

    if (scancode == 0x50) {
        if (history_pos != 0 && history_count > 0 && !line_reading) {
            int next = (history_pos + 1) % HISTORY_SIZE;
            if (next != history_tail || history_count == HISTORY_SIZE) history_pos = next;
            else history_pos = 0;
            while (cmd_index > 0) { backspace(); cmd_index--; }
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

    if (scancode == 0x0E) {
        if (cmd_index > 0) { cmd_index--; backspace(); }
    } else {
        char ascii = scancode_to_ascii(scancode);
        if (ascii != 0) {
            if (ascii == '\n') {
                putchar('\n');
                __asm__ volatile("mov $0x20, %%al\n out %%al, $0x20" : : : "eax");
                if (line_reading) {
                    strcpy_safe(line_buffer, cmd_buffer, CMD_BUFFER_SIZE);
                    line_ready = 1;
                    cmd_index = 0;
                    cmd_buffer[0] = '\0';
                    disable_hardware_cursor();
                    return;
                }
                cmd_buffer[cmd_index] = '\0';
                process_command();
                disable_hardware_cursor();
                return;
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

void setup_idt(void) {
    ip.limit = sizeof(idt) - 1;
    ip.base  = (uint32_t)&idt;

    /* 默认所有中断/异常都指向 exception_handler_wrapper */
    uint32_t eh = (uint32_t)exception_handler_wrapper;
    for (int i = 0; i < 256; i++) {
        idt[i].base_low  = eh & 0xFFFF;
        idt[i].sel       = 0x08;
        idt[i].always0   = 0;
        idt[i].flags     = 0x8E;
        idt[i].base_high = (eh >> 16) & 0xFFFF;
    }

    /* 键盘 IRQ1 (0x21) */
    uint32_t handler = (uint32_t)keyboard_handler_wrapper;
    idt[0x21].base_low  = handler & 0xFFFF;
    idt[0x21].sel       = 0x08;
    idt[0x21].always0   = 0;
    idt[0x21].flags     = 0x8E;
    idt[0x21].base_high = (handler >> 16) & 0xFFFF;

    /* 鼠标 IRQ12 (0x2C) */
    uint32_t mhandler = (uint32_t)mouse_handler_wrapper;
    idt[0x2C].base_low  = mhandler & 0xFFFF;
    idt[0x2C].sel       = 0x08;
    idt[0x2C].always0   = 0;
    idt[0x2C].flags     = 0x8E;
    idt[0x2C].base_high = (mhandler >> 16) & 0xFFFF;

    __asm__ volatile("lidt (%0)" : : "r" (&ip));
}

void setup_pic(void) {
    __asm__ volatile(
        "mov $0x11, %%al\n out %%al, $0x20\n out %%al, $0xA0\n"
        "mov $0x20, %%al\n out %%al, $0x21\n"
        "mov $0x28, %%al\n out %%al, $0xA1\n"
        "mov $0x04, %%al\n out %%al, $0x21\n"
        "mov $0x02, %%al\n out %%al, $0xA1\n"
        "mov $0x01, %%al\n out %%al, $0x21\n out %%al, $0xA1\n"
        : : : "eax"
    );
    __asm__ volatile(
        "mov $0xF9, %%al\n out %%al, $0x21\n"
        "mov $0xEF, %%al\n out %%al, $0xA1\n"
        : : : "eax"
    );
    outb(0x21, inb(0x21) | 0x01);
}

// ================== 内核入口 ==================
__attribute__((noreturn))
void kmain(unsigned int magic, unsigned int addr) {
    (void)magic;
    (void)addr;
    
    // 如果 magic 是 Multiboot 的魔数，说明是从 ISO/GRUB 启动的，允许安装
    // 如果是 0，说明是从硬盘 MBR+Stage2 启动的，禁用安装
    if (magic == 0x2BADB002) {
        allow_install = 1;
    } else {
        allow_install = 0;
    }

    __asm__ volatile("cli");
    clear_screen();

        __asm__ volatile("cli");            /* 初始化全程关中断 */

    clear_screen();
    print("[boot] gdt...\n");
    setup_gdt();                        /* ← 1. 先 GDT */
    print("[boot] pic...\n");
    setup_pic();                        /* ← 2. 再 PIC（重映射 + 屏蔽时钟） */
    print("[boot] idt...\n");
    setup_idt();                        /* ← 3. 最后装 IDT */

    print("[boot] mouse...\n");
    mouse_init();

    print("[boot] probing disk...\n");
    g_disk_sectors = ata_identify_sectors();
    {
        char b[32];
        print("Disk sectors: ");
        print(itoa((int)g_disk_sectors, b));
        print(" (");
        print(itoa((int)(g_disk_sectors / 2048), b));
        print(" MB)\n");
    }

    if (!fat32_mount()) {
        print("No FAT32 filesystem found.\n");
        if (g_disk_sectors <= FAT32_VOLUME_OFFSET + 0x10000) {
            print("ERROR: disk too small for FAT32 (need >=32MB + 1MB offset).\n");
        } else {
            uint32_t sz = g_disk_sectors - FAT32_VOLUME_OFFSET;
            if (sz > 0x1000000) sz = 0x1000000;
            fat32_format(sz);
        }
    }

    mouse_disable();

    clear_screen();
    disable_hardware_cursor();

    print("  _   _                     ___   _____ \n");
    print(" | \\ | |                   / _ \\ / ____|\n");
    print(" |  \\| | _____   _____ _ _| | | | (___  \n");
    print(" | . ` |/ _ \\ \\ / / _ \\ '__| | | |\\___ \\ \n");
    print(" | |\\  | (_) \\ V /  __/ |  | |_| |____) |\n");
    print(" |_| \\_|\\___/ \\_/ \\___|_|   \\___/|_____/ \n");
    print("\n");
    print("           Nova OS v0.8\n");

    print("Commands: ls, mkdir, cd, pwd, touch, cat, write, rm,\n");
    print("          cls, help, reboot, version, echo, beep,\n");
    print("          shutdown, calc, format, mouse on/off, cxx , Install \n");
    print("> ");

    __asm__ volatile("sti");           /* banner 打完才开中断 */
    while (1) { __asm__ volatile("hlt"); }
}