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

int  fat32_mount(void);
void fat32_format(uint32_t total_sectors);
extern void mouse_handler_wrapper();
void mouse_init(void);
void mouse_handler_c(void);
// ================== 全局变量 ==================
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

#define HISTORY_SIZE 16
static char history[HISTORY_SIZE][CMD_BUFFER_SIZE];
static int history_count = 0;
static int history_pos = 0;
static int history_tail = 0;
#define FAT32_VOLUME_OFFSET 2048   // 1MB 偏移，避开 MBR
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

// 用端口 0x80 做短延迟（每次 inb 约 1us）
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
    uint32_t timeout = 200000;          // 缩短超时，避免卡死
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

// 软复位：正确时序 + 等待 10ms 让设备稳定
static void ata_soft_reset(void) {
    outb(0x3F6, 0x06);          // SRST=1, nIEN=1
    ata_delay_us(5);
    outb(0x3F6, 0x02);          // SRST=0, nIEN=1
    ata_delay_ms(10);           // 关键：等够时间再发命令
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
    if (!ata_wait_drq()) {
        ata_soft_reset();
        return 0;
    }
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

// ================== 缓存磁盘容量 ==================
static uint32_t g_disk_sectors = 0;

uint32_t ata_disk_sectors(void) {
    return g_disk_sectors;
}

static uint32_t ata_identify_sectors(void) {
    for (int attempt = 0; attempt < 3; attempt++) {
        // 先等 50ms 让驱动器上电稳定
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

        // 更长超时：1000 万次轮询
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

// ================== FAT32 全局状态 ==================
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

// ================== FAT32 挂载 / 格式化 ==================
int fat32_mount(void) {
    uint8_t boot[ATA_SECTOR_SIZE];
    if (!ata_read_sector(FAT32_VOLUME_OFFSET, boot)) return 0;

    if (boot[510] != 0x55 || boot[511] != 0xAA) return 0;
    struct fat32_bpb* bpb = (struct fat32_bpb*)boot;

    if (bpb->bytes_per_sector != 512) return 0;
    if (bpb->sectors_per_fat_32 == 0) return 0;
    if (bpb->sectors_per_cluster == 0 ||
        bpb->fat_count == 0 ||
        bpb->reserved_sectors == 0) return 0;

    fat32_sectors_per_fat = bpb->sectors_per_fat_32;
    fat32_root_cluster    = bpb->root_cluster;
    fat32_fat_start       = FAT32_VOLUME_OFFSET + bpb->reserved_sectors;
    fat32_data_start      = FAT32_VOLUME_OFFSET + bpb->reserved_sectors
                          + bpb->fat_count * fat32_sectors_per_fat;
    fat32_total_sectors   = bpb->total_sectors_32 ?
                            bpb->total_sectors_32 : bpb->total_sectors_16;
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

    if (total_sectors < 0x10000) {
        print("FAT32: volume too small.\n");
        goto done;
    }
    print("Formatting FAT32...\n");

    uint32_t spc = 8;
    uint32_t reserved = 32;
    uint32_t fat_count = 2;

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
    boot[510] = 0x55;
    boot[511] = 0xAA;

    if (!ata_write_sector(FAT32_VOLUME_OFFSET + 0, boot)) {
        print("FAT32: boot write failed\n");
        goto done;
    }

    uint8_t zero[ATA_SECTOR_SIZE];
    for (int i = 0; i < ATA_SECTOR_SIZE; i++) zero[i] = 0;
    for (uint32_t i = 1; i < reserved; i++) {
        if (!ata_write_sector(FAT32_VOLUME_OFFSET + i, zero)) {
            print("FAT32: reserved write failed\n");
            goto done;
        }
    }

    uint8_t fat1[ATA_SECTOR_SIZE];
    for (int i = 0; i < ATA_SECTOR_SIZE; i++) fat1[i] = 0;
    fat1[0]=0xF8; fat1[1]=0xFF; fat1[2]=0xFF; fat1[3]=0x0F;
    fat1[4]=0xFF; fat1[5]=0xFF; fat1[6]=0xFF; fat1[7]=0x0F;
    fat1[8]=0xFF; fat1[9]=0xFF; fat1[10]=0xFF; fat1[11]=0x0F;

    uint32_t fat1_start = reserved;
    uint32_t fat2_start = reserved + fat_sectors;

    if (!ata_write_sector(FAT32_VOLUME_OFFSET + fat1_start, fat1) ||
        !ata_write_sector(FAT32_VOLUME_OFFSET + fat2_start, fat1)) {
        print("FAT32: FAT header write failed\n");
        goto done;
    }

    for (int i = 0; i < ATA_SECTOR_SIZE; i++) fat1[i] = 0;
    for (uint32_t i = 1; i < fat_sectors; i++) {
        if (!ata_write_sector(FAT32_VOLUME_OFFSET + fat1_start + i, fat1) ||
            !ata_write_sector(FAT32_VOLUME_OFFSET + fat2_start + i, fat1)) {
            print("FAT32: FAT body write failed\n");
            goto done;
        }
    }

    uint32_t data_start = reserved + fat_count * fat_sectors;
    for (uint32_t s = 0; s < spc; s++) {
        if (!ata_write_sector(FAT32_VOLUME_OFFSET + data_start + s, zero)) {
            print("FAT32: root cluster write failed\n");
            goto done;
        }
    }

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

// ================== FAT32 目录 / 文件操作 ==================
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
                for (int k = 0; k < 11; k++) {
                    if (entries[idx].name[k] != name83[k]) { match = 0; break; }
                }
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

                    if (!ata_write_sector(sector, buf)) {
                        print("FAT32: dir write failed\n");
                        return -1;
                    }
                    uint8_t verify[ATA_SECTOR_SIZE];
                    if (!ata_read_sector(sector, verify)) {
                        print("FAT32: verify read failed\n");
                        return -1;
                    }
                    if (((struct fat32_dir_entry*)verify)[idx].name[0] != name83[0]) {
                        print("FAT32: verify mismatch\n");
                        return -1;
                    }
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
            for (int k = 0; k < ATA_SECTOR_SIZE; k++) {
                buf[k] = (written < size) ? data[written++] : 0;
            }
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
    for (int i = 0; i < 80 * 25; i++) {
        video[2*i] = ' ';
        video[2*i+1] = 0x07;
    }
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

void print(const char* str) {
    while (*str) { putchar(*str); str++; }
}

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
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
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
// ================== PS/2 鼠标 ==================
static int mouse_px = 320;              // 像素坐标 [0..639]
static int mouse_py = 240;              // 像素坐标 [0..479]
static int mouse_char_x = 40;           // 字符坐标 [0..79]
static int mouse_char_y = 15;           // 字符坐标 [0..24]
static int mouse_old_x = -1;
static int mouse_old_y = -1;
static int mouse_visible = 0;
static char mouse_saved_char = ' ';
static uint8_t mouse_saved_attr = 0x07;

static uint8_t mouse_cycle = 0;
static uint8_t mouse_byte[3];

static void mouse_wait_write(void) {
    uint32_t timeout = 1000000;
    while (timeout--) {
        if (!(inb(0x64) & 0x02)) return;
    }
}

static void mouse_write_cmd(uint8_t val) {
    mouse_wait_write();
    outb(0x64, 0xD4);       // 下一个字节发给鼠标
    mouse_wait_write();
    outb(0x60, val);
}

static uint8_t mouse_read_byte(void) {
    uint32_t timeout = 1000000;
    while (timeout--) {
        if (inb(0x64) & 0x01) return inb(0x60);
    }
    return 0xFF;
}

static void mouse_draw(void) {
    char* video = (char*)0xB8000;

    // 还原旧位置
    if (mouse_visible && mouse_old_x >= 0 && mouse_old_y >= 0) {
        int pos = mouse_old_y * 80 + mouse_old_x;
        video[2*pos] = mouse_saved_char;
        video[2*pos+1] = mouse_saved_attr;
    }

    // 保存新位置的字符，画一个反色块
    int pos = mouse_char_y * 80 + mouse_char_x;
    mouse_saved_char = video[2*pos];
    mouse_saved_attr = video[2*pos+1];
    video[2*pos] = ' ';
    video[2*pos+1] = 0x70;      // 黑底亮灰（反色）

    mouse_visible = 1;
    mouse_old_x = mouse_char_x;
    mouse_old_y = mouse_char_y;
}

void mouse_handler_c(void) {
    uint8_t data = inb(0x60);

    switch (mouse_cycle) {
        case 0:
            // 第一个字节必须 bit3 = 1，否则丢弃这个字节重新同步
            if (data & 0x08) {
                mouse_byte[0] = data;
                mouse_cycle = 1;
            }
            break;
        case 1:
            mouse_byte[1] = data;
            mouse_cycle = 2;
            break;
        case 2:
            mouse_byte[2] = data;
            mouse_cycle = 0;

            {
                int dx = (int)(signed char)mouse_byte[1];
                int dy = (int)(signed char)mouse_byte[2];

                mouse_px += dx;
                mouse_py -= dy;         // 鼠标 Y 向上为正，屏幕 Y 向下为正

                if (mouse_px < 0) mouse_px = 0;
                if (mouse_px > 639) mouse_px = 639;
                if (mouse_py < 0) mouse_py = 0;
                if (mouse_py > 479) mouse_py = 479;

                int cx = mouse_px / 8;      // 8 像素 ≈ 1 个字符宽
                int cy = mouse_py / 16;     // 16 像素 ≈ 1 个字符高
                if (cx != mouse_char_x || cy != mouse_char_y) {
                    mouse_char_x = cx;
                    mouse_char_y = cy;
                    mouse_draw();
                }
            }
            break;
    }

    // 给从 PIC 发 EOI（IRQ12 挂在从 PIC 上）
    outb(0xA0, 0x20);
    // 给主 PIC 发 EOI
    outb(0x20, 0x20);
}

void mouse_init(void) {
    // 1. 启用辅助 PS/2 端口（鼠标）
    mouse_wait_write();
    outb(0x64, 0xA8);

    // 2. 读配置字节，启用鼠标中断，清掉鼠标时钟禁用位
    mouse_wait_write();
    outb(0x64, 0x20);
    uint8_t status = mouse_read_byte();
    status |= 0x02;         // 启用鼠标中断
    status &= ~0x20;        // 清掉鼠标时钟禁用
    mouse_wait_write();
    outb(0x64, 0x60);
    mouse_wait_write();
    outb(0x60, status);

    // 3. 让鼠标使用默认参数
    mouse_write_cmd(0xF6);
    mouse_read_byte();      // 期望 ACK = 0xFA

    // 4. 启用数据上报
    mouse_write_cmd(0xF4);
    mouse_read_byte();      // 期望 ACK

    mouse_draw();
}
// ================== 处理命令 ==================
static void process_command(void) {
    // -------- ls --------
    if (strcmp(cmd_buffer, "ls") == 1) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        uint32_t cluster = fat32_cur_cluster;
        uint8_t buf[ATA_SECTOR_SIZE];
        int count = 0;
        int stop = 0;
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
                    if (entries[i].attributes & ATTR_DIRECTORY) {
                        print("[DIR]  ");
                    } else {
                        print("[FILE] ");
                    }
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
    // -------- mkdir --------
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
        if (!ata_write_sector(base, dirbuf)) {
            print("Error: write failed\n"); goto done;
        }
        for (int i = 0; i < ATA_SECTOR_SIZE; i++) dirbuf[i] = 0;
        for (uint32_t s = 1; s < fat32_spc; s++) ata_write_sector(base + s, dirbuf);

        if (fat32_create_in_dir(fat32_cur_cluster, dname,
                                ATTR_DIRECTORY, new_c, 0) != 0) {
            fat32_free_chain(new_c);
            print("Error: cannot create\n");
            goto done;
        }
        print("Directory created.\n");
    }
    // -------- cd --------
    else if (starts_with(cmd_buffer, "cd ")) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* path = cmd_buffer + 3;
        while (*path == ' ') path++;
        if (*path == '\0' || strcmp(path, "/") == 1) {
            fat32_cur_cluster = fat32_root_cluster;
            strcpy_safe(fat32_cur_path, "/", 256);
            print("Changed to /\n");
            goto done;
        }
        if (strcmp(path, "..") == 1) {
            if (fat32_cur_cluster == fat32_root_cluster) {
                print("Already at root.\n");
                goto done;
            }
            uint8_t buf[ATA_SECTOR_SIZE];
            if (!ata_read_sector(fat32_cluster_sector(fat32_cur_cluster), buf)) {
                print("Error: read failed\n"); goto done;
            }
            struct fat32_dir_entry* entries = (struct fat32_dir_entry*)buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == '.' && entries[i].name[1] == '.') {
                    uint32_t p = dir_cluster(&entries[i]);
                    if (p < 2) p = fat32_root_cluster;
                    fat32_cur_cluster = p;
                    print("Changed to ..\n");
                    goto done;
                }
                if (entries[i].name[0] == 0x00) break;
            }
            print("Error: no parent\n");
            goto done;
        }
        struct fat32_dir_entry entry;
        if (fat32_find_in_dir(fat32_cur_cluster, path, &entry, 0, 0) != 0) {
            print("Directory not found.\n"); goto done;
        }
        if (!(entry.attributes & ATTR_DIRECTORY)) {
            print("Not a directory.\n"); goto done;
        }
        uint32_t c = dir_cluster(&entry);
        if (c < 2) c = fat32_root_cluster;
        fat32_cur_cluster = c;
        print("Changed to: ");
        print(path);
        print("\n");
    }
    // -------- pwd --------
    else if (strcmp(cmd_buffer, "pwd") == 1) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        if (fat32_cur_cluster == fat32_root_cluster) print("/\n");
        else {
            print("/");
            print(fat32_cur_path);
            print("\n");
        }
    }
    // -------- touch --------
    else if (starts_with(cmd_buffer, "touch ")) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* fname = cmd_buffer + 6;
        while (*fname == ' ') fname++;
        if (*fname == '\0') { print("Error: missing name\n"); goto done; }
        struct fat32_dir_entry tmp;
        if (fat32_find_in_dir(fat32_cur_cluster, fname, &tmp, 0, 0) == 0) {
            print("File already exists.\n"); goto done;
        }
        if (fat32_create_in_dir(fat32_cur_cluster, fname,
                                ATTR_ARCHIVE, 0, 0) != 0) {
            print("Error: cannot create\n"); goto done;
        }
        print("File created.\n");
    }
    // -------- cat --------
    else if (starts_with(cmd_buffer, "cat ")) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* fname = cmd_buffer + 4;
        while (*fname == ' ') fname++;
        struct fat32_dir_entry entry;
        if (fat32_find_in_dir(fat32_cur_cluster, fname, &entry, 0, 0) != 0) {
            print("File not found.\n"); goto done;
        }
        if (entry.attributes & ATTR_DIRECTORY) {
            print("Is a directory.\n"); goto done;
        }
        if (entry.file_size == 0) { print("(empty)\n"); goto done; }
        static uint8_t data[8192];
        uint32_t max = sizeof(data) - 1;
        uint32_t got = fat32_read_file(&entry, data, max);
        data[got] = '\0';
        print((char*)data);
        print("\n");
    }
    // -------- write --------
    else if (starts_with(cmd_buffer, "write ")) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* rest = cmd_buffer + 6;
        while (*rest == ' ') rest++;
        char fname[64];
        int i = 0;
        while (*rest != ' ' && *rest != '\0' && i < 63) fname[i++] = *rest++;
        fname[i] = '\0';
        while (*rest == ' ') rest++;
        if (*rest == '\0') { print("Error: no content\n"); goto done; }

        struct fat32_dir_entry entry;
        uint32_t sector; int idx;
        if (fat32_find_in_dir(fat32_cur_cluster, fname, &entry, &sector, &idx) != 0) {
            print("File not found.\n"); goto done;
        }
        if (entry.attributes & ATTR_DIRECTORY) {
            print("Is a directory.\n"); goto done;
        }
        int len = 0;
        while (rest[len]) len++;
        if (fat32_overwrite_file(sector, idx, (const uint8_t*)rest, (uint32_t)len) != 0) {
            print("Error: write failed\n"); goto done;
        }
        char b[16];
        print("Written ");
        print(itoa(len, b));
        print(" bytes.\n");
    }
    // -------- rm --------
    else if (starts_with(cmd_buffer, "rm ")) {
        if (!fat32_mounted) { print("No FAT32 filesystem.\n"); goto done; }
        const char* fname = cmd_buffer + 3;
        while (*fname == ' ') fname++;
        struct fat32_dir_entry entry;
        uint32_t sector; int idx;
        if (fat32_find_in_dir(fat32_cur_cluster, fname, &entry, &sector, &idx) != 0) {
            print("Not found.\n"); goto done;
        }
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
        if (!ata_read_sector(sector, dbuf)) {
            print("Error: read failed\n"); goto done;
        }
        ((struct fat32_dir_entry*)dbuf)[idx].name[0] = 0xE5;
        ata_write_sector(sector, dbuf);
        print("Deleted.\n");
    }
    // -------- format --------
    else if (starts_with(cmd_buffer, "format")) {
        uint32_t disk = ata_disk_sectors();
        if (disk == 0) { print("No disk detected.\n"); goto done; }
        if (disk <= FAT32_VOLUME_OFFSET + 0x10000) {
            print("Disk too small.\n"); goto done;
        }
        uint32_t vol = disk - FAT32_VOLUME_OFFSET;
        if (vol > 0x1000000) vol = 0x1000000;
        fat32_format(vol);
        print("Format complete.\n");
    }
    // -------- cls --------
    else if (strcmp(cmd_buffer, "cls") == 1) {
        clear_screen();
    }
    // -------- help --------
    else if (strcmp(cmd_buffer, "help") == 1) {
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
        print("cat <name>              - Show file\n");
        print("write <name> <content>  - Write to file\n");
        print("rm <name>               - Delete file/empty dir\n");
        print("format                  - Reformat as FAT32\n");
        print("Tip: up/down for history\n");
    }
    else if (strcmp(cmd_buffer, "reboot") == 1) {
        print("Rebooting...\n");
        __asm__ volatile("cli");

        // 方法 1：PIIX4 芯片组的复位控制寄存器（VMware 首选）
        uint8_t rcr = inb(0xCF9);
        outb(0xCF9, (rcr & 0xF9) | 0x02);   // SYS_RST = 热复位
        outb(0xCF9, (rcr & 0xF9) | 0x06);   // SYS_RST + RC_RST 触发

        for (volatile int i = 0; i < 10000000; i++) { }

        // 方法 2：键盘控制器复位（后备）
        for (int i = 0; i < 100000; i++) {
            if (!(inb(0x64) & 0x02)) break;
        }
        outb(0x64, 0xFE);

        for (volatile int i = 0; i < 10000000; i++) { }

        // 方法 3：三重故障（最终兜底）
        struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = {0, 0};
        __asm__ volatile("lidt (%0)" : : "r"(&null_idt));
        __asm__ volatile("int $0x03");

        while (1) { __asm__ volatile("hlt"); }
    }
    else if (strcmp(cmd_buffer, "version") == 1) {
        print("Nova OS Kernel v0.7 (LBA28 + FAT32)\n");
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
        print("System halted.\n");
        __asm__ volatile("cli\n hlt");
        while (1);
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
struct gdt_ptr {
    uint16_t limit; uint32_t base;
} __attribute__((packed));

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
struct idt_ptr {
    uint16_t limit; uint32_t base;
} __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr ip;

extern void keyboard_handler_wrapper();

void keyboard_handler_c(void) {
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

    if (scancode == 0x48) {
        if (history_count > 0) {
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
        if (history_pos != 0 && history_count > 0) {
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

void setup_idt(void) {
    ip.limit = sizeof(idt) - 1;
    ip.base = (uint32_t)&idt;
    for (int i = 0; i < 256; i++) {
        idt[i].base_low = 0;
        idt[i].sel = 0x08;
        idt[i].always0 = 0;
        idt[i].flags = 0x8E;
        idt[i].base_high = 0;
    }
    uint32_t handler = (uint32_t)keyboard_handler_wrapper;
    idt[0x21].base_low  = handler & 0xFFFF;
    idt[0x21].sel       = 0x08;
    idt[0x21].always0   = 0;
    idt[0x21].flags     = 0x8E;
    idt[0x21].base_high = (handler >> 16) & 0xFFFF;
    idt[0x21].base_high = (handler >> 16) & 0xFFFF;
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
}

// ================== 内核入口 ==================
__attribute__((noreturn))
void kmain(unsigned int magic, unsigned int addr) {
    (void)magic; (void)addr;

    // ---- 1. 早期初始化：先建立能打印诊断信息的通道 ----
    clear_screen();
    print("[boot] gdt...\n");
    setup_gdt();
    print("[boot] idt...\n");
    setup_idt();
    print("[boot] pic...\n");
    setup_pic();
    print("[boot] mouse...\n");
    mouse_init();
    print("[boot] done\n");

    // ---- 2. 只探测一次磁盘，结果缓存进 g_disk_sectors ----
    //        这一处是唯一一次 IDENTIFY，避免短时间内重发命令读到垃圾值
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

    // ---- 3. 挂载已有 FAT32；失败则视容量决定是否格式化 ----
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

    // ---- 4. 清屏，打印 logo，进入 shell，开中断 ----
    clear_screen();
    disable_hardware_cursor();

    print("  _   _                     ___   _____ \n");
    print(" | \\ | |                   / _ \\ / ____|\n");
    print(" |  \\| | _____   _____ _ _| | | | (___  \n");
    print(" | . ` |/ _ \\ \\ / / _ \\ '__| | | |\\___ \\ \n");
    print(" | |\\  | (_) \\ V /  __/ |  | |_| |____) |\n");
    print(" |_| \\_|\\___/ \\_/ \\___|_|   \\___/|_____/ \n");
    print("\n");
    print("           Nova OS v0.7 (LBA28 + FAT32)\n");

    print("Commands: ls, mkdir, cd, pwd, touch, cat, write, rm,\n");
    print("          cls, help, reboot, version, echo, beep,\n");
    print("          shutdown, calc, format\n");
    print("> ");

    __asm__ volatile("sti");

    while (1) { }
}
