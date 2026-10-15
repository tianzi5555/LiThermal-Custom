"""给 AmebaPro2 SDK 打补丁：让 BW21 能做「代理 ARP + IP 转发」的 USB 网卡网关。

在 workflow 里 SDK 检出后、编译前执行：
    python3 bw21/tools/apply_patches.py <sdk_root>

做三件事（全部幂等，重复执行安全）：
  1. component/lwip/api/lwipopts.h   打开 IP_FORWARD
  2. component/lwip/lwip_v2.1.2/src/core/ipv4/etharp.c  加代理 ARP 钩子
  3. component/lwip/lwip_v2.1.2/src/core/ipv4/ip4.c     加相机主机路由钩子
钩子函数本体在自定义例程里实现（li_gw_proxy_arp / li_gw_host_route），
并且只有定义了 LI_THERMAL_GW 宏时才生效（由例程的 cmake 全局定义）。
"""
import io, os, sys

MARK = "LI_THERMAL_GW"

def read(p):
    return io.open(p, encoding="utf-8", errors="surrogateescape").read()

def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="\n", errors="surrogateescape").write(s)

def patch_file(path, hunks, label):
    if not os.path.exists(path):
        print(f"!! 找不到 {path}")
        return False
    s = read(path)
    changed = 0
    for old, new in hunks:
        if new in s:
            print(f"   [{label}] 已经打过这个补丁，跳过")
            continue
        if old not in s:
            print(f"   [{label}] !! 找不到待替换片段，跳过：{old.strip().splitlines()[0][:70]}")
            continue
        if s.count(old) != 1:
            print(f"   [{label}] !! 待替换片段出现 {s.count(old)} 次，跳过（避免误改）")
            continue
        s = s.replace(old, new, 1)
        changed += 1
    if changed:
        write(path, s)
    print(f"   [{label}] 应用 {changed} 处")
    return True

def main(sdk):
    print(f"SDK: {sdk}")

    # ---------- 1. lwipopts.h：打开 IP_FORWARD ----------
    opt = os.path.join(sdk, r"component/lwip/api/lwipopts.h")
    if os.path.exists(opt):
        s = read(opt)
        if "#define IP_FORWARD 1" in s or "IP_FORWARD      1" in s:
            print("   [lwipopts] IP_FORWARD 已经是 1")
        else:
            s += ("\n/* LiThermal: USB 网卡网关需要 IP 转发（相机 ←→ WiFi 之间转发） */\n"
                  "#undef IP_FORWARD\n#define IP_FORWARD 1\n")
            write(opt, s)
            print("   [lwipopts] 已追加 #define IP_FORWARD 1")
    else:
        print(f"!! 找不到 {opt}")

    # ---------- 2. etharp.c：代理 ARP ----------
    e = os.path.join(sdk, r"component/lwip/lwip_v2.1.2/src/core/ipv4/etharp.c")
    hunk_a = (
        "\t\t/* ARP packet directed to us? */\n"
        "\t\tfor_us = (u8_t)ip4_addr_cmp(&dipaddr, netif_ip4_addr(netif));\n",
        "\t\t/* ARP packet directed to us? */\n"
        "\t\tfor_us = (u8_t)ip4_addr_cmp(&dipaddr, netif_ip4_addr(netif));\n"
        f"#ifdef {MARK}\n"
        "\t\t/* LiThermal: 代理 ARP —— 相机挂在 ECM 口后面，要替它回答 ARP 请求 */\n"
        "\t\textern int li_gw_proxy_arp(struct netif *netif, const ip4_addr_t *target);\n"
        "\t\tif (!for_us && li_gw_proxy_arp(netif, &dipaddr)) {\n"
        "\t\t\tfor_us = 1;\n"
        "\t\t}\n"
        "#endif\n",
    )
    hunk_b = (
        "\t\tif (for_us) {\n"
        "\t\t\t/* send ARP response */\n"
        "\t\t\tetharp_raw(netif,\n"
        "\t\t\t\t\t   (struct eth_addr *)netif->hwaddr, &hdr->shwaddr,\n"
        "\t\t\t\t\t   (struct eth_addr *)netif->hwaddr, netif_ip4_addr(netif),\n"
        "\t\t\t\t\t   &hdr->shwaddr, &sipaddr,\n"
        "\t\t\t\t\t   ARP_REPLY);\n",
        "\t\tif (for_us) {\n"
        "\t\t\t/* send ARP response */\n"
        f"#ifdef {MARK}\n"
        "\t\t\t/* LiThermal: 代理 ARP 时回包里的“发送方 IP”必须是被代理的那个 IP，\n"
        "\t\t\t   否则请求方学到的还是我们自己的地址，流量不会送到我们这里 */\n"
        "\t\t\t{\n"
        "\t\t\t\tconst ip4_addr_t *li_reply_ip = netif_ip4_addr(netif);\n"
        "\t\t\t\tif (!ip4_addr_cmp(&dipaddr, netif_ip4_addr(netif)) && li_gw_proxy_arp(netif, &dipaddr)) {\n"
        "\t\t\t\t\tli_reply_ip = &dipaddr;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tetharp_raw(netif,\n"
        "\t\t\t\t\t\t   (struct eth_addr *)netif->hwaddr, &hdr->shwaddr,\n"
        "\t\t\t\t\t\t   (struct eth_addr *)netif->hwaddr, li_reply_ip,\n"
        "\t\t\t\t\t\t   &hdr->shwaddr, &sipaddr,\n"
        "\t\t\t\t\t\t   ARP_REPLY);\n"
        "\t\t\t}\n"
        "#else\n"
        "\t\t\tetharp_raw(netif,\n"
        "\t\t\t\t\t   (struct eth_addr *)netif->hwaddr, &hdr->shwaddr,\n"
        "\t\t\t\t\t   (struct eth_addr *)netif->hwaddr, netif_ip4_addr(netif),\n"
        "\t\t\t\t\t   &hdr->shwaddr, &sipaddr,\n"
        "\t\t\t\t\t   ARP_REPLY);\n"
        "#endif\n",
    )
    patch_file(e, [hunk_a, hunk_b], "etharp")

    # ---------- 3. ip4.c：相机主机路由 ----------
    i4 = os.path.join(sdk, r"component/lwip/lwip_v2.1.2/src/core/ipv4/ip4.c")
    hunk_c = (
        "struct netif *\n"
        "ip4_route(const ip4_addr_t *dest)\n"
        "{\n"
        "#if !LWIP_SINGLE_NETIF\n"
        "  struct netif *netif;\n"
        "\n"
        "  LWIP_ASSERT_CORE_LOCKED();\n",
        "struct netif *\n"
        "ip4_route(const ip4_addr_t *dest)\n"
        "{\n"
        "#if !LWIP_SINGLE_NETIF\n"
        "  struct netif *netif;\n"
        "\n"
        "  LWIP_ASSERT_CORE_LOCKED();\n"
        f"#ifdef {MARK}\n"
        "  /* LiThermal: 相机挂在 USB-ECM 口上，却用家里网段的地址；\n"
        "     没有这条主机路由，发往相机的包会因为“同网段”而从 WiFi 口出去 */\n"
        "  {\n"
        "    extern struct netif *li_gw_host_route(const ip4_addr_t *dest);\n"
        "    struct netif *li_r = li_gw_host_route(dest);\n"
        "    if (li_r != NULL) {\n"
        "      return li_r;\n"
        "    }\n"
        "  }\n"
        "#endif\n",
    )
    patch_file(i4, [hunk_c], "ip4")

    # ---------- 4. usbh_cdc_ecm.c：读不到 MAC 字符串时退化成固定 MAC ----------
    # Linux 的 f_ecm 不提供 iMACAddress 指向的字符串描述符（或长度不符合），
    # 原代码会一直停在 CDC_ECM_STATE_GET_MAC_STR 重试：
    #   [ECMH] Get MAC fail error[3]  （无限刷屏，ECM 永远起不来，还会疯狂 detach 打乱 USB）
    # 退化成固定 MAC 后 mac_valid=1，状态机继续往下走，链路才建得起来。
    # 对端 MAC 是靠 ARP 学到的，本端 MAC 只要自洽即可。
    ecm = os.path.join(sdk, r"component/usb/usb_stack/v1.0.0/host/cdc_ecm/usbh_cdc_ecm.c")
    hunk_d = (
        "\t} else {\n"
        "\t\tstatus = usbh_cdc_ecm_process_get_string(host, cdc->dongle_ctrl_buf, USBH_CDC_ECM_MAC_STRING_LEN, cdc->iMACAddressStringId);\n"
        "\t\tcdc->mac_src_type = CDC_ECM_MAC_DONGLE_SUPPLY;\n"
        "\t}\n",
        "\t} else {\n"
        "\t\tstatus = usbh_cdc_ecm_process_get_string(host, cdc->dongle_ctrl_buf, USBH_CDC_ECM_MAC_STRING_LEN, cdc->iMACAddressStringId);\n"
        "\t\tcdc->mac_src_type = CDC_ECM_MAC_DONGLE_SUPPLY;\n"
        f"#ifdef {MARK}\n"
        "\t\t/* LiThermal: Linux f_ecm 不带 MAC 字符串描述符，读它会失败；\n"
        "\t\t   原代码会一直重试导致 ECM 永远起不来。这里退化成固定 MAC 继续往下走。 */\n"
        "\t\tif (status != HAL_OK) {\n"
        "\t\t\tcdc->mac[0] = 0x02;\n"
        "\t\t\tcdc->mac[1] = 0x1f;\n"
        "\t\t\tcdc->mac[2] = 0x3a;\n"
        "\t\t\tcdc->mac[3] = 0x00;\n"
        "\t\t\tcdc->mac[4] = 0x00;\n"
        "\t\t\tcdc->mac[5] = 0x07;\n"
        "\t\t\tcdc->mac_src_type = CDC_ECM_MAC_RANDOM_SET;\n"
        "\t\t\tcdc->mac_valid = 1;\n"
        "\t\t\tRTK_LOGS(TAG, RTK_LOG_INFO, \"LiGW: no MAC string descriptor, use fixed mac\\n\");\n"
        "\t\t\tstatus = HAL_OK;\n"
        "\t\t}\n"
        "#endif\n"
        "\t}\n",
    )
    patch_file(ecm, [hunk_d], "cdc_ecm")

    print("补丁完成")
    return 0

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法: python3 apply_patches.py <sdk_root>")
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
