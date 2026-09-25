/*
 * netinfo.h - 「网络」页的数据源：zte-agent 的 /api/netinfo（出口 IP 和归属地、
 * 原始/注册运营商、漫游、自动/手动选网、搜网结果、选网保护、邻区）。
 *
 * 慢的事（查归属地、搜网、选网保护、扫邻区）全在 agent 的后台线程里做，
 * 这边只发起、然后轮询 /api/netinfo。设计见 manager 仓库
 * docs/designs/netinfo-screen.md。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_NETINFO_H
#define U60_NETINFO_H

#define NI_MAX_OPS    8
#define NI_MAX_CELLS  8
#define NI_MAX_SCENES 6
#define NI_MAX_CLIENTS 8
#define NI_MAX_APNS   10

typedef struct {
    int  present;           /* agent 给了这一项（CHILL 没开时 proxy 不给） */
    char ip[48];
    char geo[64];
    char isp[48];
    char node[64];          /* 只有 CHILL 出口有：当前节点 */
    char err[80];
} ni_exit_t;

typedef struct {
    char name[48];
    char country[24];
    char mcc[4], mnc[4];
} ni_oper_t;

typedef struct {
    char plmn[8];
    char name[48];
    char country[24];
    char rat[8];            /* 原样传回 /api/modem/register */
    char status[4];         /* AT+COPS 惯例：1 可用 2 当前 3 禁止（未实测） */
} ni_scan_op_t;

typedef struct {
    char rat[4], pci[8], arfcn[12], rsrp[8], rsrq[8], sinr[8];
} ni_cell_t;

typedef struct {
    char name[40];          /* DHCP 名字，没有就空 */
    char mac[20];
    char ip[20];
    long long down, up;     /* 连上以来的累计字节（设备下载 / 上传） */
    long down_rate, up_rate;/* 字节/秒；-1 = 还没有两次读数 */
    int  signal;            /* dBm，0 = 不知道 */
    char band[12];          /* "2.4 GHz" / "5 GHz" / "6 GHz"，不知道就空（写 GHz，免得和蜂窝 5G 混） */
    int  wifi_gen;          /* 7/6/5/4，0 = 不知道 */
    int  link_down;         /* 协商速率 Mbit/s，0 = 不知道 */
} ni_client_t;

/* 一个 APN（agent：/api/netinfo 的 apn 段） */
typedef struct {
    char id[24];            /* profileId："auto109590" / "manu1" */
    char name[40];
    char apn[40];
    int  pdp;               /* 1 IPv4  2 IPv6  3 IPv4v6 */
    int  selected;          /* 手动列表：手动模式会用这条（不等于正在用） */
    int  in_use;            /* 数据连接拨的就是它 */
} ni_apn_t;

typedef struct {
    char id[24];
    char name[32];
    int  wifi_off;          /* 这个情景会关 Wi-Fi（在家） */
    int  abroad;
    char when[112];         /* 什么时候进入（agent 按配置写好的中文） */
    char does[112];         /* 进入后改什么 */
} ni_scene_t;

typedef struct {
    char err[96];           /* "" = 正常 */
    long now;

    ni_exit_t direct, proxy;
    int  chill_running;

    ni_oper_t home, serving;
    int  roaming;           /* 1 漫游 0 本地 -1 不知道 */
    char selection[12];     /* auto / manual / "" */

    char guard_phase[16];   /* idle registering ok reverting reverted revert_failed */
    char guard_target[8];
    char guard_reason[96];

    char scan_state[12];    /* idle scanning done error */
    char scan_err[80];
    int  nops;
    ni_scan_op_t ops[NI_MAX_OPS];

    char nbr_state[12];
    char nbr_err[80];
    long nbr_at;
    int  ncells;
    ni_cell_t cells[NI_MAX_CELLS];

    /* 情景（手动固定用；自动判定的结果首页情景卡已经有） */
    int  scene_known;       /* agent 给了 scenes 段 */
    int  scene_enabled;
    int  scene_takeover;    /* Wi-Fi 看门狗接管中：固定暂停生效 */
    char scene_current[24];
    char scene_pin[24];     /* "" = 自动 */
    int  nscenes;
    ni_scene_t scenes[NI_MAX_SCENES];

    /* 每台 Wi-Fi 设备的流量（agent 读 iw station dump，只在这页开着时读） */
    int  clients_known;
    int  nclients;
    ni_client_t clients[NI_MAX_CLIENTS];

    /* APN：正在拨的那条、自动/手动、已存的手动 APN（只在完整读取里有） */
    int  apn_known;
    int  apn_manual;        /* 1 = 手动模式 */
    ni_apn_t apn_in_use;    /* id 空 = 读不到 */
    int  napns;
    ni_apn_t apns[NI_MAX_APNS];   /* 手动列表 */
} netinfo_t;

/* 主循环每轮调用。mode：NI_OFF 没人看；NI_PAGE 「情景 · 网络」页开着（刚打开
 * 立即读，之后 5 秒一次，搜网/选网/扫邻区进行中 2 秒一次）；NI_HOME 首页开着
 * （30 秒一次，精简读取：agent 不为它读选网方式）。返回 1 = 内容变了。 */
/* NI_PAGE：完整读取（运营商选择页，会让 agent 读 AT+COPS?）。其余只读 agent 缓存：
 * NI_LITE 情景 / 小区信息 / 出口；NI_APN 蜂窝 / APN（多读 APN）；NI_CLIENTS Wi-Fi
 * （让 agent 继续读每台设备的流量）。这几种 5 秒一次，NI_HOME 30 秒一次。 */
enum { NI_OFF = 0, NI_PAGE = 1, NI_HOME = 2, NI_LITE = 3, NI_APN = 4, NI_CLIENTS = 5 };
int netinfo_poll(int mode);
const netinfo_t *netinfo_get(void);

/* 动作：都只是发起，结果从下一轮 netinfo_poll 里看。 */
void netinfo_scan(void);            /* 搜索网络（会断网 1–3 分钟） */
void netinfo_register(int op);      /* 注册到 ops[op]，失败 agent 自己回自动 */
void netinfo_auto(void);            /* 恢复自动选网 */
void netinfo_nbr_scan(void);        /* 扫一次邻区 */
void netinfo_pin(const char *id);   /* 固定到这个情景；NULL = 回到自动 */
void netinfo_apn_use(const char *id);   /* "auto" 或手动 APN 的 id；会短暂断网 */
/* 刚发了动作、在等结果：接下来 secs 秒按忙时节奏（2 秒）重读 */
void netinfo_hurry(int secs);
const char *netinfo_action_error(void);   /* 上一个动作被拒的原因，"" = 没有 */

#endif /* U60_NETINFO_H */
