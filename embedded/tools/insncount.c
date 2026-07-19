/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * insncount.c -- QEMU TCG plugin: count retired guest instructions and
 * print the total at exit. QEMU is not cycle-accurate; these are
 * dynamic instruction counts, useful for order-of-magnitude cost and
 * for ISA-vs-ISA ratios (RV32I vs RV32IM, M0+ vs M4), not for cycles.
 *
 * Build (host compiler, needs qemu-plugin.h and glib headers):
 *   make plugin
 */

#include <stdio.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static struct qemu_plugin_scoreboard *counts;

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    (void)id;
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64,
        qemu_plugin_scoreboard_u64(counts),
        qemu_plugin_tb_n_insns(tb));
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    (void)id; (void)p;
    uint64_t total = 0;
    for (int i = 0; i < qemu_plugin_num_vcpus(); i++)
        total += qemu_plugin_u64_get(qemu_plugin_scoreboard_u64(counts), i);
    char buf[80];
    snprintf(buf, sizeof buf, "guest-insns: %llu\n",
             (unsigned long long)total);
    qemu_plugin_outs(buf);
    qemu_plugin_scoreboard_free(counts);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    (void)info; (void)argc; (void)argv;
    counts = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
