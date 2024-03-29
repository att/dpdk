/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/queue.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_cycles.h>

#include <rte_interrupts.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_pci.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_alarm.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_atomic.h>
#include <rte_malloc.h>
#include <rte_random.h>
#include <rte_dev.h>

#include "ixgbe_logs.h"
#include "base/ixgbe_api.h"
#include "base/ixgbe_vf.h"
#include "base/ixgbe_common.h"
#include "ixgbe_ethdev.h"
#include "ixgbe_bypass.h"
#include "ixgbe_rxtx.h"
#include "base/ixgbe_type.h"
#include "base/ixgbe_phy.h"
#include "ixgbe_regs.h"

/*
 * High threshold controlling when to start sending XOFF frames. Must be at
 * least 8 bytes less than receive packet buffer size. This value is in units
 * of 1024 bytes.
 */
#define IXGBE_FC_HI    0x80

/*
 * Low threshold controlling when to start sending XON frames. This value is
 * in units of 1024 bytes.
 */
#define IXGBE_FC_LO    0x40

/* Default minimum inter-interrupt interval for EITR configuration */
#define IXGBE_MIN_INTER_INTERRUPT_INTERVAL_DEFAULT    0x79E

/* Timer value included in XOFF frames. */
#define IXGBE_FC_PAUSE 0x680

#define IXGBE_LINK_DOWN_CHECK_TIMEOUT 4000 /* ms */
#define IXGBE_LINK_UP_CHECK_TIMEOUT   1000 /* ms */
#define IXGBE_VMDQ_NUM_UC_MAC         4096 /* Maximum nb. of UC MAC addr. */

#define IXGBE_MMW_SIZE_DEFAULT        0x4
#define IXGBE_MMW_SIZE_JUMBO_FRAME    0x14
#define IXGBE_MAX_RING_DESC           4096 /* replicate define from rxtx */

/*
 *  Default values for RX/TX configuration
 */
#define IXGBE_DEFAULT_RX_FREE_THRESH  32
#define IXGBE_DEFAULT_RX_PTHRESH      8
#define IXGBE_DEFAULT_RX_HTHRESH      8
#define IXGBE_DEFAULT_RX_WTHRESH      0

#define IXGBE_DEFAULT_TX_FREE_THRESH  32
#define IXGBE_DEFAULT_TX_PTHRESH      32
#define IXGBE_DEFAULT_TX_HTHRESH      0
#define IXGBE_DEFAULT_TX_WTHRESH      0
#define IXGBE_DEFAULT_TX_RSBIT_THRESH 32

/* Bit shift and mask */
#define IXGBE_4_BIT_WIDTH  (CHAR_BIT / 2)
#define IXGBE_4_BIT_MASK   RTE_LEN2MASK(IXGBE_4_BIT_WIDTH, uint8_t)
#define IXGBE_8_BIT_WIDTH  CHAR_BIT
#define IXGBE_8_BIT_MASK   UINT8_MAX

#define IXGBEVF_PMD_NAME "rte_ixgbevf_pmd" /* PMD name */

#define IXGBE_QUEUE_STAT_COUNTERS (sizeof(hw_stats->qprc) / sizeof(hw_stats->qprc[0]))

#define IXGBE_HKEY_MAX_INDEX 10

/* Additional timesync values. */
#define NSEC_PER_SEC             1000000000L
#define IXGBE_INCVAL_10GB        0x66666666
#define IXGBE_INCVAL_1GB         0x40000000
#define IXGBE_INCVAL_100         0x50000000
#define IXGBE_INCVAL_SHIFT_10GB  28
#define IXGBE_INCVAL_SHIFT_1GB   24
#define IXGBE_INCVAL_SHIFT_100   21
#define IXGBE_INCVAL_SHIFT_82599 7
#define IXGBE_INCPER_SHIFT_82599 24

#define IXGBE_CYCLECOUNTER_MASK   0xffffffffffffffffULL

static int eth_ixgbe_dev_init(struct rte_eth_dev *eth_dev);
static int eth_ixgbe_dev_uninit(struct rte_eth_dev *eth_dev);
static int  ixgbe_dev_configure(struct rte_eth_dev *dev);
static int  ixgbe_dev_start(struct rte_eth_dev *dev);
static void ixgbe_dev_stop(struct rte_eth_dev *dev);
static int  ixgbe_dev_set_link_up(struct rte_eth_dev *dev);
static int  ixgbe_dev_set_link_down(struct rte_eth_dev *dev);
static void ixgbe_dev_close(struct rte_eth_dev *dev);
static void ixgbe_dev_promiscuous_enable(struct rte_eth_dev *dev);
static void ixgbe_dev_promiscuous_disable(struct rte_eth_dev *dev);
static void ixgbe_dev_allmulticast_enable(struct rte_eth_dev *dev);
static void ixgbe_dev_allmulticast_disable(struct rte_eth_dev *dev);
static int ixgbe_dev_link_update(struct rte_eth_dev *dev,
				int wait_to_complete);
static void ixgbe_dev_stats_get(struct rte_eth_dev *dev,
				struct rte_eth_stats *stats);
static int ixgbe_dev_xstats_get(struct rte_eth_dev *dev,
				struct rte_eth_xstats *xstats, unsigned n);
static int ixgbevf_dev_xstats_get(struct rte_eth_dev *dev,
				  struct rte_eth_xstats *xstats, unsigned n);
static void ixgbe_dev_stats_reset(struct rte_eth_dev *dev);
static void ixgbe_dev_xstats_reset(struct rte_eth_dev *dev);
static int ixgbe_dev_queue_stats_mapping_set(struct rte_eth_dev *eth_dev,
					     uint16_t queue_id,
					     uint8_t stat_idx,
					     uint8_t is_rx);
static void ixgbe_dev_info_get(struct rte_eth_dev *dev,
			       struct rte_eth_dev_info *dev_info);
static void ixgbevf_dev_info_get(struct rte_eth_dev *dev,
				 struct rte_eth_dev_info *dev_info);
static int ixgbe_dev_mtu_set(struct rte_eth_dev *dev, uint16_t mtu);

static int ixgbe_vlan_filter_set(struct rte_eth_dev *dev,
		uint16_t vlan_id, int on);
static void ixgbe_vlan_tpid_set(struct rte_eth_dev *dev, uint16_t tpid_id);
static void ixgbe_vlan_hw_strip_bitmap_set(struct rte_eth_dev *dev,
		uint16_t queue, bool on);
static void ixgbe_vlan_strip_queue_set(struct rte_eth_dev *dev, uint16_t queue,
		int on);
static void ixgbe_vlan_offload_set(struct rte_eth_dev *dev, int mask);
static void ixgbe_vlan_hw_strip_enable(struct rte_eth_dev *dev, uint16_t queue);
static void ixgbe_vlan_hw_strip_disable(struct rte_eth_dev *dev, uint16_t queue);
static void ixgbe_vlan_hw_extend_enable(struct rte_eth_dev *dev);
static void ixgbe_vlan_hw_extend_disable(struct rte_eth_dev *dev);

static int ixgbe_dev_led_on(struct rte_eth_dev *dev);
static int ixgbe_dev_led_off(struct rte_eth_dev *dev);
static int ixgbe_flow_ctrl_get(struct rte_eth_dev *dev,
			       struct rte_eth_fc_conf *fc_conf);
static int ixgbe_flow_ctrl_set(struct rte_eth_dev *dev,
			       struct rte_eth_fc_conf *fc_conf);
static int ixgbe_priority_flow_ctrl_set(struct rte_eth_dev *dev,
		struct rte_eth_pfc_conf *pfc_conf);
static int ixgbe_dev_rss_reta_update(struct rte_eth_dev *dev,
			struct rte_eth_rss_reta_entry64 *reta_conf,
			uint16_t reta_size);
static int ixgbe_dev_rss_reta_query(struct rte_eth_dev *dev,
			struct rte_eth_rss_reta_entry64 *reta_conf,
			uint16_t reta_size);
static void ixgbe_dev_link_status_print(struct rte_eth_dev *dev);
static int ixgbe_dev_lsc_interrupt_setup(struct rte_eth_dev *dev);
static int ixgbe_dev_rxq_interrupt_setup(struct rte_eth_dev *dev);
static int ixgbe_dev_interrupt_get_status(struct rte_eth_dev *dev);
static int ixgbe_dev_interrupt_action(struct rte_eth_dev *dev);
static void ixgbe_dev_interrupt_handler(struct rte_intr_handle *handle,
		void *param);
static void ixgbe_dev_interrupt_delayed_handler(void *param);
static void ixgbe_add_rar(struct rte_eth_dev *dev, struct ether_addr *mac_addr,
		uint32_t index, uint32_t pool);
static void ixgbe_remove_rar(struct rte_eth_dev *dev, uint32_t index);
static void ixgbe_set_default_mac_addr(struct rte_eth_dev *dev,
					   struct ether_addr *mac_addr);
static void ixgbe_dcb_init(struct ixgbe_hw *hw,struct ixgbe_dcb_config *dcb_config);

/* For Virtual Function support */
static int eth_ixgbevf_dev_init(struct rte_eth_dev *eth_dev);
static int eth_ixgbevf_dev_uninit(struct rte_eth_dev *eth_dev);
static int  ixgbevf_dev_configure(struct rte_eth_dev *dev);
static int  ixgbevf_dev_start(struct rte_eth_dev *dev);
static void ixgbevf_dev_stop(struct rte_eth_dev *dev);
static void ixgbevf_dev_close(struct rte_eth_dev *dev);
static void ixgbevf_intr_disable(struct ixgbe_hw *hw);
static void ixgbevf_intr_enable(struct ixgbe_hw *hw);
static void ixgbevf_dev_stats_get(struct rte_eth_dev *dev,
		struct rte_eth_stats *stats);
static void ixgbevf_dev_stats_reset(struct rte_eth_dev *dev);
static int ixgbevf_vlan_filter_set(struct rte_eth_dev *dev,
		uint16_t vlan_id, int on);
static void ixgbevf_vlan_strip_queue_set(struct rte_eth_dev *dev,
		uint16_t queue, int on);
static void ixgbevf_vlan_offload_set(struct rte_eth_dev *dev, int mask);
static void ixgbevf_set_vfta_all(struct rte_eth_dev *dev, bool on);
static int ixgbevf_dev_rx_queue_intr_enable(struct rte_eth_dev *dev,
					    uint16_t queue_id);
static int ixgbevf_dev_rx_queue_intr_disable(struct rte_eth_dev *dev,
					     uint16_t queue_id);
static void ixgbevf_set_ivar_map(struct ixgbe_hw *hw, int8_t direction,
				 uint8_t queue, uint8_t msix_vector);
static void ixgbevf_configure_msix(struct rte_eth_dev *dev);

/* For Eth VMDQ APIs support */
static int ixgbe_uc_hash_table_set(struct rte_eth_dev *dev, struct
		ether_addr* mac_addr,uint8_t on);
static int ixgbe_uc_all_hash_table_set(struct rte_eth_dev *dev,uint8_t on);
static int  ixgbe_set_pool_rx_mode(struct rte_eth_dev *dev,  uint16_t pool,
		uint16_t rx_mask, uint8_t on);
static int ixgbe_set_pool_rx(struct rte_eth_dev *dev,uint16_t pool,uint8_t on);
static int ixgbe_set_pool_tx(struct rte_eth_dev *dev,uint16_t pool,uint8_t on);
static int ixgbe_set_pool_vlan_filter(struct rte_eth_dev *dev, uint16_t vlan,
		uint64_t pool_mask,uint8_t vlan_on);
static int ixgbe_set_pool_vlan_anti_spoof(struct rte_eth_dev *dev,
		uint32_t vf, uint8_t on);
static int ixgbe_set_pool_mac_anti_spoof(struct rte_eth_dev *dev,
		uint32_t vf, uint8_t on);
static int ixgbe_ping_vfs(struct rte_eth_dev *dev, int32_t vf);
static int ixgbe_mirror_rule_set(struct rte_eth_dev *dev,
		struct rte_eth_mirror_conf *mirror_conf,
		uint8_t rule_id, uint8_t on);
static int ixgbe_mirror_rule_reset(struct rte_eth_dev *dev,
		uint8_t	rule_id);
static int ixgbe_dev_rx_queue_intr_enable(struct rte_eth_dev *dev,
					  uint16_t queue_id);
static int ixgbe_dev_rx_queue_intr_disable(struct rte_eth_dev *dev,
					   uint16_t queue_id);
static void ixgbe_set_ivar_map(struct ixgbe_hw *hw, int8_t direction,
			       uint8_t queue, uint8_t msix_vector);
static void ixgbe_configure_msix(struct rte_eth_dev *dev);

static int ixgbe_set_queue_rate_limit(struct rte_eth_dev *dev,
		uint16_t queue_idx, uint16_t tx_rate);
static int ixgbe_set_vf_rate_limit(struct rte_eth_dev *dev, uint16_t vf,
		uint16_t tx_rate, uint64_t q_msk);

static void ixgbevf_add_mac_addr(struct rte_eth_dev *dev,
				 struct ether_addr *mac_addr,
				 uint32_t index, uint32_t pool);
static void ixgbevf_remove_mac_addr(struct rte_eth_dev *dev, uint32_t index);
static void ixgbevf_set_default_mac_addr(struct rte_eth_dev *dev,
					     struct ether_addr *mac_addr);
static int ixgbe_syn_filter_set(struct rte_eth_dev *dev,
			struct rte_eth_syn_filter *filter,
			bool add);
static int ixgbe_syn_filter_get(struct rte_eth_dev *dev,
			struct rte_eth_syn_filter *filter);
static int ixgbe_syn_filter_handle(struct rte_eth_dev *dev,
			enum rte_filter_op filter_op,
			void *arg);
static int ixgbe_add_5tuple_filter(struct rte_eth_dev *dev,
			struct ixgbe_5tuple_filter *filter);
static void ixgbe_remove_5tuple_filter(struct rte_eth_dev *dev,
			struct ixgbe_5tuple_filter *filter);
static int ixgbe_add_del_ntuple_filter(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *filter,
			bool add);
static int ixgbe_ntuple_filter_handle(struct rte_eth_dev *dev,
				enum rte_filter_op filter_op,
				void *arg);
static int ixgbe_get_ntuple_filter(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *filter);
static int ixgbe_add_del_ethertype_filter(struct rte_eth_dev *dev,
			struct rte_eth_ethertype_filter *filter,
			bool add);
static int ixgbe_ethertype_filter_handle(struct rte_eth_dev *dev,
				enum rte_filter_op filter_op,
				void *arg);
static int ixgbe_get_ethertype_filter(struct rte_eth_dev *dev,
			struct rte_eth_ethertype_filter *filter);
static int ixgbe_dev_filter_ctrl(struct rte_eth_dev *dev,
		     enum rte_filter_type filter_type,
		     enum rte_filter_op filter_op,
		     void *arg);
static int ixgbevf_dev_set_mtu(struct rte_eth_dev *dev, uint16_t mtu);

static int ixgbe_dev_set_mc_addr_list(struct rte_eth_dev *dev,
				      struct ether_addr *mc_addr_set,
				      uint32_t nb_mc_addr);
static int ixgbe_dev_get_dcb_info(struct rte_eth_dev *dev,
				   struct rte_eth_dcb_info *dcb_info);

static int ixgbe_get_reg_length(struct rte_eth_dev *dev);
static int ixgbe_get_regs(struct rte_eth_dev *dev,
			    struct rte_dev_reg_info *regs);
static int ixgbe_get_eeprom_length(struct rte_eth_dev *dev);
static int ixgbe_get_eeprom(struct rte_eth_dev *dev,
				struct rte_dev_eeprom_info *eeprom);
static int ixgbe_set_eeprom(struct rte_eth_dev *dev,
				struct rte_dev_eeprom_info *eeprom);

static int ixgbevf_get_reg_length(struct rte_eth_dev *dev);
static int ixgbevf_get_regs(struct rte_eth_dev *dev,
				struct rte_dev_reg_info *regs);

static int ixgbe_timesync_enable(struct rte_eth_dev *dev);
static int ixgbe_timesync_disable(struct rte_eth_dev *dev);
static int ixgbe_timesync_read_rx_timestamp(struct rte_eth_dev *dev,
					    struct timespec *timestamp,
					    uint32_t flags);
static int ixgbe_timesync_read_tx_timestamp(struct rte_eth_dev *dev,
					    struct timespec *timestamp);
static int ixgbe_timesync_adjust_time(struct rte_eth_dev *dev, int64_t delta);
static int ixgbe_timesync_read_time(struct rte_eth_dev *dev,
				   struct timespec *timestamp);
static int ixgbe_timesync_write_time(struct rte_eth_dev *dev,
				   const struct timespec *timestamp);

/*
 * Define VF Stats MACRO for Non "cleared on read" register
 */
#define UPDATE_VF_STAT(reg, last, cur)                          \
{                                                               \
	uint32_t latest = IXGBE_READ_REG(hw, reg);              \
	cur += (latest - last) & UINT_MAX;                      \
	last = latest;                                          \
}

#define UPDATE_VF_STAT_36BIT(lsb, msb, last, cur)                \
{                                                                \
	u64 new_lsb = IXGBE_READ_REG(hw, lsb);                   \
	u64 new_msb = IXGBE_READ_REG(hw, msb);                   \
	u64 latest = ((new_msb << 32) | new_lsb);                \
	cur += (0x1000000000LL + latest - last) & 0xFFFFFFFFFLL; \
	last = latest;                                           \
}

#define IXGBE_SET_HWSTRIP(h, q) do{\
		uint32_t idx = (q) / (sizeof ((h)->bitmap[0]) * NBBY); \
		uint32_t bit = (q) % (sizeof ((h)->bitmap[0]) * NBBY); \
		(h)->bitmap[idx] |= 1 << bit;\
	}while(0)

#define IXGBE_CLEAR_HWSTRIP(h, q) do{\
		uint32_t idx = (q) / (sizeof ((h)->bitmap[0]) * NBBY); \
		uint32_t bit = (q) % (sizeof ((h)->bitmap[0]) * NBBY); \
		(h)->bitmap[idx] &= ~(1 << bit);\
	}while(0)

#define IXGBE_GET_HWSTRIP(h, q, r) do{\
		uint32_t idx = (q) / (sizeof ((h)->bitmap[0]) * NBBY); \
		uint32_t bit = (q) % (sizeof ((h)->bitmap[0]) * NBBY); \
		(r) = (h)->bitmap[idx] >> bit & 1;\
	}while(0)

/*
 * The set of PCI devices this driver supports
 */
static const struct rte_pci_id pci_id_ixgbe_map[] = {

#define RTE_PCI_DEV_ID_DECL_IXGBE(vend, dev) {RTE_PCI_DEVICE(vend, dev)},
#include "rte_pci_dev_ids.h"

{ .vendor_id = 0, /* sentinel */ },
};


/*
 * The set of PCI devices this driver supports (for 82599 VF)
 */
static const struct rte_pci_id pci_id_ixgbevf_map[] = {

#define RTE_PCI_DEV_ID_DECL_IXGBEVF(vend, dev) {RTE_PCI_DEVICE(vend, dev)},
#include "rte_pci_dev_ids.h"
{ .vendor_id = 0, /* sentinel */ },

};

static const struct rte_eth_desc_lim rx_desc_lim = {
	.nb_max = IXGBE_MAX_RING_DESC,
	.nb_min = IXGBE_MIN_RING_DESC,
	.nb_align = IXGBE_RXD_ALIGN,
};

static const struct rte_eth_desc_lim tx_desc_lim = {
	.nb_max = IXGBE_MAX_RING_DESC,
	.nb_min = IXGBE_MIN_RING_DESC,
	.nb_align = IXGBE_TXD_ALIGN,
};

static const struct eth_dev_ops ixgbe_eth_dev_ops = {
	.dev_configure        = ixgbe_dev_configure,
	.dev_start            = ixgbe_dev_start,
	.dev_stop             = ixgbe_dev_stop,
	.dev_set_link_up    = ixgbe_dev_set_link_up,
	.dev_set_link_down  = ixgbe_dev_set_link_down,
	.dev_close            = ixgbe_dev_close,
	.promiscuous_enable   = ixgbe_dev_promiscuous_enable,
	.promiscuous_disable  = ixgbe_dev_promiscuous_disable,
	.allmulticast_enable  = ixgbe_dev_allmulticast_enable,
	.allmulticast_disable = ixgbe_dev_allmulticast_disable,
	.link_update          = ixgbe_dev_link_update,
	.stats_get            = ixgbe_dev_stats_get,
	.xstats_get           = ixgbe_dev_xstats_get,
	.stats_reset          = ixgbe_dev_stats_reset,
	.xstats_reset         = ixgbe_dev_xstats_reset,
	.queue_stats_mapping_set = ixgbe_dev_queue_stats_mapping_set,
	.dev_infos_get        = ixgbe_dev_info_get,
	.mtu_set              = ixgbe_dev_mtu_set,
	.vlan_filter_set      = ixgbe_vlan_filter_set,
	.vlan_tpid_set        = ixgbe_vlan_tpid_set,
	.vlan_offload_set     = ixgbe_vlan_offload_set,
	.vlan_strip_queue_set = ixgbe_vlan_strip_queue_set,
	.rx_queue_start	      = ixgbe_dev_rx_queue_start,
	.rx_queue_stop        = ixgbe_dev_rx_queue_stop,
	.tx_queue_start	      = ixgbe_dev_tx_queue_start,
	.tx_queue_stop        = ixgbe_dev_tx_queue_stop,
	.rx_queue_setup       = ixgbe_dev_rx_queue_setup,
	.rx_queue_intr_enable = ixgbe_dev_rx_queue_intr_enable,
	.rx_queue_intr_disable = ixgbe_dev_rx_queue_intr_disable,
	.rx_queue_release     = ixgbe_dev_rx_queue_release,
	.rx_queue_count       = ixgbe_dev_rx_queue_count,
	.rx_descriptor_done   = ixgbe_dev_rx_descriptor_done,
	.tx_queue_setup       = ixgbe_dev_tx_queue_setup,
	.tx_queue_release     = ixgbe_dev_tx_queue_release,
	.dev_led_on           = ixgbe_dev_led_on,
	.dev_led_off          = ixgbe_dev_led_off,
	.flow_ctrl_get        = ixgbe_flow_ctrl_get,
	.flow_ctrl_set        = ixgbe_flow_ctrl_set,
	.priority_flow_ctrl_set = ixgbe_priority_flow_ctrl_set,
	.mac_addr_add         = ixgbe_add_rar,
	.mac_addr_remove      = ixgbe_remove_rar,
	.mac_addr_set         = ixgbe_set_default_mac_addr,
	.uc_hash_table_set    = ixgbe_uc_hash_table_set,
	.uc_all_hash_table_set  = ixgbe_uc_all_hash_table_set,
	.mirror_rule_set      = ixgbe_mirror_rule_set,
	.mirror_rule_reset    = ixgbe_mirror_rule_reset,
	.set_vf_rx_mode       = ixgbe_set_pool_rx_mode,
	.set_vf_rx            = ixgbe_set_pool_rx,
	.set_vf_tx            = ixgbe_set_pool_tx,
	.set_vf_vlan_filter   = ixgbe_set_pool_vlan_filter,
	.set_vf_vlan_anti_spoof  = ixgbe_set_pool_vlan_anti_spoof,
	.set_vf_mac_anti_spoof   = ixgbe_set_pool_mac_anti_spoof,
	.ping_vfs             = ixgbe_ping_vfs,
	.set_queue_rate_limit = ixgbe_set_queue_rate_limit,
	.set_vf_rate_limit    = ixgbe_set_vf_rate_limit,
	.reta_update          = ixgbe_dev_rss_reta_update,
	.reta_query           = ixgbe_dev_rss_reta_query,
#ifdef RTE_NIC_BYPASS
	.bypass_init          = ixgbe_bypass_init,
	.bypass_state_set     = ixgbe_bypass_state_store,
	.bypass_state_show    = ixgbe_bypass_state_show,
	.bypass_event_set     = ixgbe_bypass_event_store,
	.bypass_event_show    = ixgbe_bypass_event_show,
	.bypass_wd_timeout_set  = ixgbe_bypass_wd_timeout_store,
	.bypass_wd_timeout_show = ixgbe_bypass_wd_timeout_show,
	.bypass_ver_show      = ixgbe_bypass_ver_show,
	.bypass_wd_reset      = ixgbe_bypass_wd_reset,
#endif /* RTE_NIC_BYPASS */
	.rss_hash_update      = ixgbe_dev_rss_hash_update,
	.rss_hash_conf_get    = ixgbe_dev_rss_hash_conf_get,
	.filter_ctrl          = ixgbe_dev_filter_ctrl,
	.set_mc_addr_list     = ixgbe_dev_set_mc_addr_list,
	.rxq_info_get         = ixgbe_rxq_info_get,
	.txq_info_get         = ixgbe_txq_info_get,
	.timesync_enable      = ixgbe_timesync_enable,
	.timesync_disable     = ixgbe_timesync_disable,
	.timesync_read_rx_timestamp = ixgbe_timesync_read_rx_timestamp,
	.timesync_read_tx_timestamp = ixgbe_timesync_read_tx_timestamp,
	.get_reg_length       = ixgbe_get_reg_length,
	.get_reg              = ixgbe_get_regs,
	.get_eeprom_length    = ixgbe_get_eeprom_length,
	.get_eeprom           = ixgbe_get_eeprom,
	.set_eeprom           = ixgbe_set_eeprom,
	.get_dcb_info         = ixgbe_dev_get_dcb_info,
	.timesync_adjust_time = ixgbe_timesync_adjust_time,
	.timesync_read_time   = ixgbe_timesync_read_time,
	.timesync_write_time  = ixgbe_timesync_write_time,
};

/*
 * dev_ops for virtual function, bare necessities for basic vf
 * operation have been implemented
 */
static const struct eth_dev_ops ixgbevf_eth_dev_ops = {
	.dev_configure        = ixgbevf_dev_configure,
	.dev_start            = ixgbevf_dev_start,
	.dev_stop             = ixgbevf_dev_stop,
	.link_update          = ixgbe_dev_link_update,
	.stats_get            = ixgbevf_dev_stats_get,
	.xstats_get           = ixgbevf_dev_xstats_get,
	.stats_reset          = ixgbevf_dev_stats_reset,
	.xstats_reset         = ixgbevf_dev_stats_reset,
	.dev_close            = ixgbevf_dev_close,
	.dev_infos_get        = ixgbevf_dev_info_get,
	.mtu_set              = ixgbevf_dev_set_mtu,
	.vlan_filter_set      = ixgbevf_vlan_filter_set,
	.vlan_strip_queue_set = ixgbevf_vlan_strip_queue_set,
	.vlan_offload_set     = ixgbevf_vlan_offload_set,
	.rx_queue_setup       = ixgbe_dev_rx_queue_setup,
	.rx_queue_release     = ixgbe_dev_rx_queue_release,
	.rx_descriptor_done   = ixgbe_dev_rx_descriptor_done,
	.tx_queue_setup       = ixgbe_dev_tx_queue_setup,
	.tx_queue_release     = ixgbe_dev_tx_queue_release,
	.rx_queue_intr_enable = ixgbevf_dev_rx_queue_intr_enable,
	.rx_queue_intr_disable = ixgbevf_dev_rx_queue_intr_disable,
	.mac_addr_add         = ixgbevf_add_mac_addr,
	.mac_addr_remove      = ixgbevf_remove_mac_addr,
	.set_mc_addr_list     = ixgbe_dev_set_mc_addr_list,
	.rxq_info_get         = ixgbe_rxq_info_get,
	.txq_info_get         = ixgbe_txq_info_get,
	.mac_addr_set         = ixgbevf_set_default_mac_addr,
	.get_reg_length       = ixgbevf_get_reg_length,
	.get_reg              = ixgbevf_get_regs,
	.reta_update          = ixgbe_dev_rss_reta_update,
	.reta_query           = ixgbe_dev_rss_reta_query,
	.rss_hash_update      = ixgbe_dev_rss_hash_update,
	.rss_hash_conf_get    = ixgbe_dev_rss_hash_conf_get,
};

/* store statistics names and its offset in stats structure */
struct rte_ixgbe_xstats_name_off {
	char name[RTE_ETH_XSTATS_NAME_SIZE];
	unsigned offset;
};

static const struct rte_ixgbe_xstats_name_off rte_ixgbe_stats_strings[] = {
	{"rx_crc_errors", offsetof(struct ixgbe_hw_stats, crcerrs)},
	{"rx_illegal_byte_errors", offsetof(struct ixgbe_hw_stats, illerrc)},
	{"rx_error_bytes", offsetof(struct ixgbe_hw_stats, errbc)},
	{"mac_local_errors", offsetof(struct ixgbe_hw_stats, mlfc)},
	{"mac_remote_errors", offsetof(struct ixgbe_hw_stats, mrfc)},
	{"rx_length_errors", offsetof(struct ixgbe_hw_stats, rlec)},
	{"tx_xon_packets", offsetof(struct ixgbe_hw_stats, lxontxc)},
	{"rx_xon_packets", offsetof(struct ixgbe_hw_stats, lxonrxc)},
	{"tx_xoff_packets", offsetof(struct ixgbe_hw_stats, lxofftxc)},
	{"rx_xoff_packets", offsetof(struct ixgbe_hw_stats, lxoffrxc)},
	{"rx_size_64_packets", offsetof(struct ixgbe_hw_stats, prc64)},
	{"rx_size_65_to_127_packets", offsetof(struct ixgbe_hw_stats, prc127)},
	{"rx_size_128_to_255_packets", offsetof(struct ixgbe_hw_stats, prc255)},
	{"rx_size_256_to_511_packets", offsetof(struct ixgbe_hw_stats, prc511)},
	{"rx_size_512_to_1023_packets", offsetof(struct ixgbe_hw_stats,
		prc1023)},
	{"rx_size_1024_to_max_packets", offsetof(struct ixgbe_hw_stats,
		prc1522)},
	{"rx_broadcast_packets", offsetof(struct ixgbe_hw_stats, bprc)},
	{"rx_multicast_packets", offsetof(struct ixgbe_hw_stats, mprc)},
	{"rx_fragment_errors", offsetof(struct ixgbe_hw_stats, rfc)},
	{"rx_undersize_errors", offsetof(struct ixgbe_hw_stats, ruc)},
	{"rx_oversize_errors", offsetof(struct ixgbe_hw_stats, roc)},
	{"rx_jabber_errors", offsetof(struct ixgbe_hw_stats, rjc)},
	{"rx_management_packets", offsetof(struct ixgbe_hw_stats, mngprc)},
	{"rx_management_dropped", offsetof(struct ixgbe_hw_stats, mngpdc)},
	{"tx_management_packets", offsetof(struct ixgbe_hw_stats, mngptc)},
	{"rx_total_packets", offsetof(struct ixgbe_hw_stats, tpr)},
	{"rx_total_bytes", offsetof(struct ixgbe_hw_stats, tor)},
	{"tx_total_packets", offsetof(struct ixgbe_hw_stats, tpt)},
	{"tx_size_64_packets", offsetof(struct ixgbe_hw_stats, ptc64)},
	{"tx_size_65_to_127_packets", offsetof(struct ixgbe_hw_stats, ptc127)},
	{"tx_size_128_to_255_packets", offsetof(struct ixgbe_hw_stats, ptc255)},
	{"tx_size_256_to_511_packets", offsetof(struct ixgbe_hw_stats, ptc511)},
	{"tx_size_512_to_1023_packets", offsetof(struct ixgbe_hw_stats,
		ptc1023)},
	{"tx_size_1024_to_max_packets", offsetof(struct ixgbe_hw_stats,
		ptc1522)},
	{"tx_multicast_packets", offsetof(struct ixgbe_hw_stats, mptc)},
	{"tx_broadcast_packets", offsetof(struct ixgbe_hw_stats, bptc)},
	{"rx_mac_short_packet_dropped", offsetof(struct ixgbe_hw_stats, mspdc)},
	{"rx_l3_l4_xsum_error", offsetof(struct ixgbe_hw_stats, xec)},

	{"flow_director_added_filters", offsetof(struct ixgbe_hw_stats,
		fdirustat_add)},
	{"flow_director_removed_filters", offsetof(struct ixgbe_hw_stats,
		fdirustat_remove)},
	{"flow_director_filter_add_errors", offsetof(struct ixgbe_hw_stats,
		fdirfstat_fadd)},
	{"flow_director_filter_remove_errors", offsetof(struct ixgbe_hw_stats,
		fdirfstat_fremove)},
	{"flow_director_matched_filters", offsetof(struct ixgbe_hw_stats,
		fdirmatch)},
	{"flow_director_missed_filters", offsetof(struct ixgbe_hw_stats,
		fdirmiss)},

	{"rx_fcoe_crc_errors", offsetof(struct ixgbe_hw_stats, fccrc)},
	{"rx_fcoe_dropped", offsetof(struct ixgbe_hw_stats, fcoerpdc)},
	{"rx_fcoe_mbuf_allocation_errors", offsetof(struct ixgbe_hw_stats,
		fclast)},
	{"rx_fcoe_packets", offsetof(struct ixgbe_hw_stats, fcoeprc)},
	{"tx_fcoe_packets", offsetof(struct ixgbe_hw_stats, fcoeptc)},
	{"rx_fcoe_bytes", offsetof(struct ixgbe_hw_stats, fcoedwrc)},
	{"tx_fcoe_bytes", offsetof(struct ixgbe_hw_stats, fcoedwtc)},
	{"rx_fcoe_no_direct_data_placement", offsetof(struct ixgbe_hw_stats,
		fcoe_noddp)},
	{"rx_fcoe_no_direct_data_placement_ext_buff",
		offsetof(struct ixgbe_hw_stats, fcoe_noddp_ext_buff)},

	{"tx_flow_control_xon_packets", offsetof(struct ixgbe_hw_stats,
		lxontxc)},
	{"rx_flow_control_xon_packets", offsetof(struct ixgbe_hw_stats,
		lxonrxc)},
	{"tx_flow_control_xoff_packets", offsetof(struct ixgbe_hw_stats,
		lxofftxc)},
	{"rx_flow_control_xoff_packets", offsetof(struct ixgbe_hw_stats,
		lxoffrxc)},
	{"rx_total_missed_packets", offsetof(struct ixgbe_hw_stats, mpctotal)},
};

#define IXGBE_NB_HW_STATS (sizeof(rte_ixgbe_stats_strings) / \
			   sizeof(rte_ixgbe_stats_strings[0]))

/* Per-queue statistics */
static const struct rte_ixgbe_xstats_name_off rte_ixgbe_rxq_strings[] = {
	{"mbuf_allocation_errors", offsetof(struct ixgbe_hw_stats, rnbc)},
	{"dropped", offsetof(struct ixgbe_hw_stats, mpc)},
	{"xon_packets", offsetof(struct ixgbe_hw_stats, pxonrxc)},
	{"xoff_packets", offsetof(struct ixgbe_hw_stats, pxoffrxc)},
};

#define IXGBE_NB_RXQ_PRIO_STATS (sizeof(rte_ixgbe_rxq_strings) / \
			   sizeof(rte_ixgbe_rxq_strings[0]))

static const struct rte_ixgbe_xstats_name_off rte_ixgbe_txq_strings[] = {
	{"xon_packets", offsetof(struct ixgbe_hw_stats, pxontxc)},
	{"xoff_packets", offsetof(struct ixgbe_hw_stats, pxofftxc)},
	{"xon_to_xoff_packets", offsetof(struct ixgbe_hw_stats,
		pxon2offc)},
};

#define IXGBE_NB_TXQ_PRIO_STATS (sizeof(rte_ixgbe_txq_strings) / \
			   sizeof(rte_ixgbe_txq_strings[0]))

static const struct rte_ixgbe_xstats_name_off rte_ixgbevf_stats_strings[] = {
	{"rx_multicast_packets", offsetof(struct ixgbevf_hw_stats, vfmprc)},
};

#define IXGBEVF_NB_XSTATS (sizeof(rte_ixgbevf_stats_strings) /	\
		sizeof(rte_ixgbevf_stats_strings[0]))

/**
 * Atomically reads the link status information from global
 * structure rte_eth_dev.
 *
 * @param dev
 *   - Pointer to the structure rte_eth_dev to read from.
 *   - Pointer to the buffer to be saved with the link status.
 *
 * @return
 *   - On success, zero.
 *   - On failure, negative value.
 */
static inline int
rte_ixgbe_dev_atomic_read_link_status(struct rte_eth_dev *dev,
				struct rte_eth_link *link)
{
	struct rte_eth_link *dst = link;
	struct rte_eth_link *src = &(dev->data->dev_link);

	if (rte_atomic64_cmpset((uint64_t *)dst, *(uint64_t *)dst,
					*(uint64_t *)src) == 0)
		return -1;

	return 0;
}

/**
 * Atomically writes the link status information into global
 * structure rte_eth_dev.
 *
 * @param dev
 *   - Pointer to the structure rte_eth_dev to read from.
 *   - Pointer to the buffer to be saved with the link status.
 *
 * @return
 *   - On success, zero.
 *   - On failure, negative value.
 */
static inline int
rte_ixgbe_dev_atomic_write_link_status(struct rte_eth_dev *dev,
				struct rte_eth_link *link)
{
	struct rte_eth_link *dst = &(dev->data->dev_link);
	struct rte_eth_link *src = link;

	if (rte_atomic64_cmpset((uint64_t *)dst, *(uint64_t *)dst,
					*(uint64_t *)src) == 0)
		return -1;

	return 0;
}

/*
 * This function is the same as ixgbe_is_sfp() in base/ixgbe.h.
 */
static inline int
ixgbe_is_sfp(struct ixgbe_hw *hw)
{
	switch (hw->phy.type) {
	case ixgbe_phy_sfp_avago:
	case ixgbe_phy_sfp_ftl:
	case ixgbe_phy_sfp_intel:
	case ixgbe_phy_sfp_unknown:
	case ixgbe_phy_sfp_passive_tyco:
	case ixgbe_phy_sfp_passive_unknown:
		return 1;
	default:
		return 0;
	}
}

static inline int32_t
ixgbe_pf_reset_hw(struct ixgbe_hw *hw)
{
	uint32_t ctrl_ext;
	int32_t status;

	status = ixgbe_reset_hw(hw);

	ctrl_ext = IXGBE_READ_REG(hw, IXGBE_CTRL_EXT);
	/* Set PF Reset Done bit so PF/VF Mail Ops can work */
	ctrl_ext |= IXGBE_CTRL_EXT_PFRSTD;
	IXGBE_WRITE_REG(hw, IXGBE_CTRL_EXT, ctrl_ext);
	IXGBE_WRITE_FLUSH(hw);

	return status;
}

static inline void
ixgbe_enable_intr(struct rte_eth_dev *dev)
{
	struct ixgbe_interrupt *intr =
		IXGBE_DEV_PRIVATE_TO_INTR(dev->data->dev_private);
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	IXGBE_WRITE_REG(hw, IXGBE_EIMS, intr->mask);
	IXGBE_WRITE_FLUSH(hw);
}

/*
 * This function is based on ixgbe_disable_intr() in base/ixgbe.h.
 */
static void
ixgbe_disable_intr(struct ixgbe_hw *hw)
{
	PMD_INIT_FUNC_TRACE();

	if (hw->mac.type == ixgbe_mac_82598EB) {
		IXGBE_WRITE_REG(hw, IXGBE_EIMC, ~0);
	} else {
		IXGBE_WRITE_REG(hw, IXGBE_EIMC, 0xFFFF0000);
		IXGBE_WRITE_REG(hw, IXGBE_EIMC_EX(0), ~0);
		IXGBE_WRITE_REG(hw, IXGBE_EIMC_EX(1), ~0);
	}
	IXGBE_WRITE_FLUSH(hw);
}

/*
 * This function resets queue statistics mapping registers.
 * From Niantic datasheet, Initialization of Statistics section:
 * "...if software requires the queue counters, the RQSMR and TQSM registers
 * must be re-programmed following a device reset.
 */
static void
ixgbe_reset_qstat_mappings(struct ixgbe_hw *hw)
{
	uint32_t i;

	for(i = 0; i != IXGBE_NB_STAT_MAPPING_REGS; i++) {
		IXGBE_WRITE_REG(hw, IXGBE_RQSMR(i), 0);
		IXGBE_WRITE_REG(hw, IXGBE_TQSM(i), 0);
	}
}


static int
ixgbe_dev_queue_stats_mapping_set(struct rte_eth_dev *eth_dev,
				  uint16_t queue_id,
				  uint8_t stat_idx,
				  uint8_t is_rx)
{
#define QSM_REG_NB_BITS_PER_QMAP_FIELD 8
#define NB_QMAP_FIELDS_PER_QSM_REG 4
#define QMAP_FIELD_RESERVED_BITS_MASK 0x0f

	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(eth_dev->data->dev_private);
	struct ixgbe_stat_mapping_registers *stat_mappings =
		IXGBE_DEV_PRIVATE_TO_STAT_MAPPINGS(eth_dev->data->dev_private);
	uint32_t qsmr_mask = 0;
	uint32_t clearing_mask = QMAP_FIELD_RESERVED_BITS_MASK;
	uint32_t q_map;
	uint8_t n, offset;

	if ((hw->mac.type != ixgbe_mac_82599EB) &&
		(hw->mac.type != ixgbe_mac_X540) &&
		(hw->mac.type != ixgbe_mac_X550) &&
		(hw->mac.type != ixgbe_mac_X550EM_x))
		return -ENOSYS;

	PMD_INIT_LOG(DEBUG, "Setting port %d, %s queue_id %d to stat index %d",
		     (int)(eth_dev->data->port_id), is_rx ? "RX" : "TX",
		     queue_id, stat_idx);

	n = (uint8_t)(queue_id / NB_QMAP_FIELDS_PER_QSM_REG);
	if (n >= IXGBE_NB_STAT_MAPPING_REGS) {
		PMD_INIT_LOG(ERR, "Nb of stat mapping registers exceeded");
		return -EIO;
	}
	offset = (uint8_t)(queue_id % NB_QMAP_FIELDS_PER_QSM_REG);

	/* Now clear any previous stat_idx set */
	clearing_mask <<= (QSM_REG_NB_BITS_PER_QMAP_FIELD * offset);
	if (!is_rx)
		stat_mappings->tqsm[n] &= ~clearing_mask;
	else
		stat_mappings->rqsmr[n] &= ~clearing_mask;

	q_map = (uint32_t)stat_idx;
	q_map &= QMAP_FIELD_RESERVED_BITS_MASK;
	qsmr_mask = q_map << (QSM_REG_NB_BITS_PER_QMAP_FIELD * offset);
	if (!is_rx)
		stat_mappings->tqsm[n] |= qsmr_mask;
	else
		stat_mappings->rqsmr[n] |= qsmr_mask;

	PMD_INIT_LOG(DEBUG, "Set port %d, %s queue_id %d to stat index %d",
		     (int)(eth_dev->data->port_id), is_rx ? "RX" : "TX",
		     queue_id, stat_idx);
	PMD_INIT_LOG(DEBUG, "%s[%d] = 0x%08x", is_rx ? "RQSMR" : "TQSM", n,
		     is_rx ? stat_mappings->rqsmr[n] : stat_mappings->tqsm[n]);

	/* Now write the mapping in the appropriate register */
	if (is_rx) {
		PMD_INIT_LOG(DEBUG, "Write 0x%x to RX IXGBE stat mapping reg:%d",
			     stat_mappings->rqsmr[n], n);
		IXGBE_WRITE_REG(hw, IXGBE_RQSMR(n), stat_mappings->rqsmr[n]);
	}
	else {
		PMD_INIT_LOG(DEBUG, "Write 0x%x to TX IXGBE stat mapping reg:%d",
			     stat_mappings->tqsm[n], n);
		IXGBE_WRITE_REG(hw, IXGBE_TQSM(n), stat_mappings->tqsm[n]);
	}
	return 0;
}

static void
ixgbe_restore_statistics_mapping(struct rte_eth_dev * dev)
{
	struct ixgbe_stat_mapping_registers *stat_mappings =
		IXGBE_DEV_PRIVATE_TO_STAT_MAPPINGS(dev->data->dev_private);
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	int i;

	/* write whatever was in stat mapping table to the NIC */
	for (i = 0; i < IXGBE_NB_STAT_MAPPING_REGS; i++) {
		/* rx */
		IXGBE_WRITE_REG(hw, IXGBE_RQSMR(i), stat_mappings->rqsmr[i]);

		/* tx */
		IXGBE_WRITE_REG(hw, IXGBE_TQSM(i), stat_mappings->tqsm[i]);
	}
}

static void
ixgbe_dcb_init(struct ixgbe_hw *hw,struct ixgbe_dcb_config *dcb_config)
{
	uint8_t i;
	struct ixgbe_dcb_tc_config *tc;
	uint8_t dcb_max_tc = IXGBE_DCB_MAX_TRAFFIC_CLASS;

	dcb_config->num_tcs.pg_tcs = dcb_max_tc;
	dcb_config->num_tcs.pfc_tcs = dcb_max_tc;
	for (i = 0; i < dcb_max_tc; i++) {
		tc = &dcb_config->tc_config[i];
		tc->path[IXGBE_DCB_TX_CONFIG].bwg_id = i;
		tc->path[IXGBE_DCB_TX_CONFIG].bwg_percent =
				 (uint8_t)(100/dcb_max_tc + (i & 1));
		tc->path[IXGBE_DCB_RX_CONFIG].bwg_id = i;
		tc->path[IXGBE_DCB_RX_CONFIG].bwg_percent =
				 (uint8_t)(100/dcb_max_tc + (i & 1));
		tc->pfc = ixgbe_dcb_pfc_disabled;
	}

	/* Initialize default user to priority mapping, UPx->TC0 */
	tc = &dcb_config->tc_config[0];
	tc->path[IXGBE_DCB_TX_CONFIG].up_to_tc_bitmap = 0xFF;
	tc->path[IXGBE_DCB_RX_CONFIG].up_to_tc_bitmap = 0xFF;
	for (i = 0; i< IXGBE_DCB_MAX_BW_GROUP; i++) {
		dcb_config->bw_percentage[IXGBE_DCB_TX_CONFIG][i] = 100;
		dcb_config->bw_percentage[IXGBE_DCB_RX_CONFIG][i] = 100;
	}
	dcb_config->rx_pba_cfg = ixgbe_dcb_pba_equal;
	dcb_config->pfc_mode_enable = false;
	dcb_config->vt_mode = true;
	dcb_config->round_robin_enable = false;
	/* support all DCB capabilities in 82599 */
	dcb_config->support.capabilities = 0xFF;

	/*we only support 4 Tcs for X540, X550 */
	if (hw->mac.type == ixgbe_mac_X540 ||
		hw->mac.type == ixgbe_mac_X550 ||
		hw->mac.type == ixgbe_mac_X550EM_x) {
		dcb_config->num_tcs.pg_tcs = 4;
		dcb_config->num_tcs.pfc_tcs = 4;
	}
}

/*
 * Ensure that all locks are released before first NVM or PHY access
 */
static void
ixgbe_swfw_lock_reset(struct ixgbe_hw *hw)
{
	uint16_t mask;

	/*
	 * Phy lock should not fail in this early stage. If this is the case,
	 * it is due to an improper exit of the application.
	 * So force the release of the faulty lock. Release of common lock
	 * is done automatically by swfw_sync function.
	 */
	mask = IXGBE_GSSR_PHY0_SM << hw->bus.func;
	if (ixgbe_acquire_swfw_semaphore(hw, mask) < 0) {
		PMD_DRV_LOG(DEBUG, "SWFW phy%d lock released", hw->bus.func);
	}
	ixgbe_release_swfw_semaphore(hw, mask);

	/*
	 * These ones are more tricky since they are common to all ports; but
	 * swfw_sync retries last long enough (1s) to be almost sure that if
	 * lock can not be taken it is due to an improper lock of the
	 * semaphore.
	 */
	mask = IXGBE_GSSR_EEP_SM | IXGBE_GSSR_MAC_CSR_SM | IXGBE_GSSR_SW_MNG_SM;
	if (ixgbe_acquire_swfw_semaphore(hw, mask) < 0) {
		PMD_DRV_LOG(DEBUG, "SWFW common locks released");
	}
	ixgbe_release_swfw_semaphore(hw, mask);
}

/*
 * This function is based on code in ixgbe_attach() in base/ixgbe.c.
 * It returns 0 on success.
 */
static int
eth_ixgbe_dev_init(struct rte_eth_dev *eth_dev)
{
	struct rte_pci_device *pci_dev;
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(eth_dev->data->dev_private);
	struct ixgbe_vfta * shadow_vfta =
		IXGBE_DEV_PRIVATE_TO_VFTA(eth_dev->data->dev_private);
	struct ixgbe_hwstrip *hwstrip =
		IXGBE_DEV_PRIVATE_TO_HWSTRIP_BITMAP(eth_dev->data->dev_private);
	struct ixgbe_dcb_config *dcb_config =
		IXGBE_DEV_PRIVATE_TO_DCB_CFG(eth_dev->data->dev_private);
	struct ixgbe_filter_info *filter_info =
		IXGBE_DEV_PRIVATE_TO_FILTER_INFO(eth_dev->data->dev_private);
	uint32_t ctrl_ext;
	uint16_t csum;
	int diag, i;

	PMD_INIT_FUNC_TRACE();

	eth_dev->dev_ops = &ixgbe_eth_dev_ops;
	eth_dev->rx_pkt_burst = &ixgbe_recv_pkts;
	eth_dev->tx_pkt_burst = &ixgbe_xmit_pkts;

	/*
	 * For secondary processes, we don't initialise any further as primary
	 * has already done this work. Only check we don't need a different
	 * RX and TX function.
	 */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY){
		struct ixgbe_tx_queue *txq;
		/* TX queue function in primary, set by last queue initialized
		 * Tx queue may not initialized by primary process */
		if (eth_dev->data->tx_queues) {
			txq = eth_dev->data->tx_queues[eth_dev->data->nb_tx_queues-1];
			ixgbe_set_tx_function(eth_dev, txq);
		} else {
			/* Use default TX function if we get here */
			PMD_INIT_LOG(NOTICE, "No TX queues configured yet. "
			                     "Using default TX function.");
		}

		ixgbe_set_rx_function(eth_dev);

		return 0;
	}
	pci_dev = eth_dev->pci_dev;

	rte_eth_copy_pci_info(eth_dev, pci_dev);

	/* Vendor and Device ID need to be set before init of shared code */
	hw->device_id = pci_dev->id.device_id;
	hw->vendor_id = pci_dev->id.vendor_id;
	hw->hw_addr = (void *)pci_dev->mem_resource[0].addr;
	hw->allow_unsupported_sfp = 1;

	/* Initialize the shared code (base driver) */
#ifdef RTE_NIC_BYPASS
	diag = ixgbe_bypass_init_shared_code(hw);
#else
	diag = ixgbe_init_shared_code(hw);
#endif /* RTE_NIC_BYPASS */

	if (diag != IXGBE_SUCCESS) {
		PMD_INIT_LOG(ERR, "Shared code init failed: %d", diag);
		return -EIO;
	}

	/* pick up the PCI bus settings for reporting later */
	ixgbe_get_bus_info(hw);

	/* Unlock any pending hardware semaphore */
	ixgbe_swfw_lock_reset(hw);

	/* Initialize DCB configuration*/
	memset(dcb_config, 0, sizeof(struct ixgbe_dcb_config));
	ixgbe_dcb_init(hw,dcb_config);
	/* Get Hardware Flow Control setting */
	hw->fc.requested_mode = ixgbe_fc_full;
	hw->fc.current_mode = ixgbe_fc_full;
	hw->fc.pause_time = IXGBE_FC_PAUSE;
	for (i = 0; i < IXGBE_DCB_MAX_TRAFFIC_CLASS; i++) {
		hw->fc.low_water[i] = IXGBE_FC_LO;
		hw->fc.high_water[i] = IXGBE_FC_HI;
	}
	hw->fc.send_xon = 1;

	/* Make sure we have a good EEPROM before we read from it */
	diag = ixgbe_validate_eeprom_checksum(hw, &csum);
	if (diag != IXGBE_SUCCESS) {
		PMD_INIT_LOG(ERR, "The EEPROM checksum is not valid: %d", diag);
		return -EIO;
	}

#ifdef RTE_NIC_BYPASS
	diag = ixgbe_bypass_init_hw(hw);
#else
	diag = ixgbe_init_hw(hw);
#endif /* RTE_NIC_BYPASS */

	/*
	 * Devices with copper phys will fail to initialise if ixgbe_init_hw()
	 * is called too soon after the kernel driver unbinding/binding occurs.
	 * The failure occurs in ixgbe_identify_phy_generic() for all devices,
	 * but for non-copper devies, ixgbe_identify_sfp_module_generic() is
	 * also called. See ixgbe_identify_phy_82599(). The reason for the
	 * failure is not known, and only occuts when virtualisation features
	 * are disabled in the bios. A delay of 100ms  was found to be enough by
	 * trial-and-error, and is doubled to be safe.
	 */
	if (diag && (hw->mac.ops.get_media_type(hw) == ixgbe_media_type_copper)) {
		rte_delay_ms(200);
		diag = ixgbe_init_hw(hw);
	}

	if (diag == IXGBE_ERR_EEPROM_VERSION) {
		PMD_INIT_LOG(ERR, "This device is a pre-production adapter/"
		    "LOM.  Please be aware there may be issues associated "
		    "with your hardware.");
		PMD_INIT_LOG(ERR, "If you are experiencing problems "
		    "please contact your Intel or hardware representative "
		    "who provided you with this hardware.");
	} else if (diag == IXGBE_ERR_SFP_NOT_SUPPORTED)
		PMD_INIT_LOG(ERR, "Unsupported SFP+ Module");
	if (diag) {
		PMD_INIT_LOG(ERR, "Hardware Initialization Failure: %d", diag);
		return -EIO;
	}

	/* Reset the hw statistics */
	ixgbe_dev_stats_reset(eth_dev);

	/* disable interrupt */
	ixgbe_disable_intr(hw);

	/* reset mappings for queue statistics hw counters*/
	ixgbe_reset_qstat_mappings(hw);

	/* Allocate memory for storing MAC addresses */
	eth_dev->data->mac_addrs = rte_zmalloc("ixgbe", ETHER_ADDR_LEN *
			hw->mac.num_rar_entries, 0);
	if (eth_dev->data->mac_addrs == NULL) {
		PMD_INIT_LOG(ERR,
			"Failed to allocate %u bytes needed to store "
			"MAC addresses",
			ETHER_ADDR_LEN * hw->mac.num_rar_entries);
		return -ENOMEM;
	}
	/* Copy the permanent MAC address */
	ether_addr_copy((struct ether_addr *) hw->mac.perm_addr,
			&eth_dev->data->mac_addrs[0]);

	/* Allocate memory for storing hash filter MAC addresses */
	eth_dev->data->hash_mac_addrs = rte_zmalloc("ixgbe", ETHER_ADDR_LEN *
			IXGBE_VMDQ_NUM_UC_MAC, 0);
	if (eth_dev->data->hash_mac_addrs == NULL) {
		PMD_INIT_LOG(ERR,
			"Failed to allocate %d bytes needed to store MAC addresses",
			ETHER_ADDR_LEN * IXGBE_VMDQ_NUM_UC_MAC);
		return -ENOMEM;
	}

	/* initialize the vfta */
	memset(shadow_vfta, 0, sizeof(*shadow_vfta));

	/* initialize the hw strip bitmap*/
	memset(hwstrip, 0, sizeof(*hwstrip));

	/* initialize PF if max_vfs not zero */
	ixgbe_pf_host_init(eth_dev);

	ctrl_ext = IXGBE_READ_REG(hw, IXGBE_CTRL_EXT);
	/* let hardware know driver is loaded */
	ctrl_ext |= IXGBE_CTRL_EXT_DRV_LOAD;
	/* Set PF Reset Done bit so PF/VF Mail Ops can work */
	ctrl_ext |= IXGBE_CTRL_EXT_PFRSTD;
	IXGBE_WRITE_REG(hw, IXGBE_CTRL_EXT, ctrl_ext);
	IXGBE_WRITE_FLUSH(hw);

	if (ixgbe_is_sfp(hw) && hw->phy.sfp_type != ixgbe_sfp_type_not_present)
		PMD_INIT_LOG(DEBUG, "MAC: %d, PHY: %d, SFP+: %d",
			     (int) hw->mac.type, (int) hw->phy.type,
			     (int) hw->phy.sfp_type);
	else
		PMD_INIT_LOG(DEBUG, "MAC: %d, PHY: %d",
			     (int) hw->mac.type, (int) hw->phy.type);

	PMD_INIT_LOG(DEBUG, "port %d vendorID=0x%x deviceID=0x%x",
			eth_dev->data->port_id, pci_dev->id.vendor_id,
			pci_dev->id.device_id);

	rte_intr_callback_register(&pci_dev->intr_handle,
				   ixgbe_dev_interrupt_handler,
				   (void *)eth_dev);

	/* enable uio/vfio intr/eventfd mapping */
	rte_intr_enable(&pci_dev->intr_handle);

	/* enable support intr */
	ixgbe_enable_intr(eth_dev);

	/* initialize 5tuple filter list */
	TAILQ_INIT(&filter_info->fivetuple_list);
	memset(filter_info->fivetuple_mask, 0,
		sizeof(uint32_t) * IXGBE_5TUPLE_ARRAY_SIZE);

	return 0;
}

static int
eth_ixgbe_dev_uninit(struct rte_eth_dev *eth_dev)
{
	struct rte_pci_device *pci_dev;
	struct ixgbe_hw *hw;

	PMD_INIT_FUNC_TRACE();

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return -EPERM;

	hw = IXGBE_DEV_PRIVATE_TO_HW(eth_dev->data->dev_private);
	pci_dev = eth_dev->pci_dev;

	if (hw->adapter_stopped == 0)
		ixgbe_dev_close(eth_dev);

	eth_dev->dev_ops = NULL;
	eth_dev->rx_pkt_burst = NULL;
	eth_dev->tx_pkt_burst = NULL;

	/* Unlock any pending hardware semaphore */
	ixgbe_swfw_lock_reset(hw);

	/* disable uio intr before callback unregister */
	rte_intr_disable(&(pci_dev->intr_handle));
	rte_intr_callback_unregister(&(pci_dev->intr_handle),
		ixgbe_dev_interrupt_handler, (void *)eth_dev);

	/* uninitialize PF if max_vfs not zero */
	ixgbe_pf_host_uninit(eth_dev);

	rte_free(eth_dev->data->mac_addrs);
	eth_dev->data->mac_addrs = NULL;

	rte_free(eth_dev->data->hash_mac_addrs);
	eth_dev->data->hash_mac_addrs = NULL;

	return 0;
}

/*
 * Negotiate mailbox API version with the PF.
 * After reset API version is always set to the basic one (ixgbe_mbox_api_10).
 * Then we try to negotiate starting with the most recent one.
 * If all negotiation attempts fail, then we will proceed with
 * the default one (ixgbe_mbox_api_10).
 */
static void
ixgbevf_negotiate_api(struct ixgbe_hw *hw)
{
	int32_t i;

	/* start with highest supported, proceed down */
	static const enum ixgbe_pfvf_api_rev sup_ver[] = {
		ixgbe_mbox_api_11,
		ixgbe_mbox_api_10,
	};

	for (i = 0;
			i != RTE_DIM(sup_ver) &&
			ixgbevf_negotiate_api_version(hw, sup_ver[i]) != 0;
			i++)
		;
}

static void
generate_random_mac_addr(struct ether_addr *mac_addr)
{
	uint64_t random;

	/* Set Organizationally Unique Identifier (OUI) prefix. */
	mac_addr->addr_bytes[0] = 0x00;
	mac_addr->addr_bytes[1] = 0x09;
	mac_addr->addr_bytes[2] = 0xC0;
	/* Force indication of locally assigned MAC address. */
	mac_addr->addr_bytes[0] |= ETHER_LOCAL_ADMIN_ADDR;
	/* Generate the last 3 bytes of the MAC address with a random number. */
	random = rte_rand();
	memcpy(&mac_addr->addr_bytes[3], &random, 3);
}

/*
 * Virtual Function device init
 */
static int
eth_ixgbevf_dev_init(struct rte_eth_dev *eth_dev)
{
	int diag;
	uint32_t tc, tcs;
	struct rte_pci_device *pci_dev;
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(eth_dev->data->dev_private);
	struct ixgbe_vfta * shadow_vfta =
		IXGBE_DEV_PRIVATE_TO_VFTA(eth_dev->data->dev_private);
	struct ixgbe_hwstrip *hwstrip =
		IXGBE_DEV_PRIVATE_TO_HWSTRIP_BITMAP(eth_dev->data->dev_private);
	struct ether_addr *perm_addr = (struct ether_addr *) hw->mac.perm_addr;

	PMD_INIT_FUNC_TRACE();

	eth_dev->dev_ops = &ixgbevf_eth_dev_ops;
	eth_dev->rx_pkt_burst = &ixgbe_recv_pkts;
	eth_dev->tx_pkt_burst = &ixgbe_xmit_pkts;

	/* for secondary processes, we don't initialise any further as primary
	 * has already done this work. Only check we don't need a different
	 * RX function */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY){
		if (eth_dev->data->scattered_rx)
			eth_dev->rx_pkt_burst = ixgbe_recv_pkts_lro_single_alloc;
		return 0;
	}

	pci_dev = eth_dev->pci_dev;

	rte_eth_copy_pci_info(eth_dev, pci_dev);

	hw->device_id = pci_dev->id.device_id;
	hw->vendor_id = pci_dev->id.vendor_id;
	hw->hw_addr = (void *)pci_dev->mem_resource[0].addr;

	/* initialize the vfta */
	memset(shadow_vfta, 0, sizeof(*shadow_vfta));

	/* initialize the hw strip bitmap*/
	memset(hwstrip, 0, sizeof(*hwstrip));

	/* Initialize the shared code (base driver) */
	diag = ixgbe_init_shared_code(hw);
	if (diag != IXGBE_SUCCESS) {
		PMD_INIT_LOG(ERR, "Shared code init failed for ixgbevf: %d", diag);
		return -EIO;
	}

	/* init_mailbox_params */
	hw->mbx.ops.init_params(hw);

	/* Reset the hw statistics */
	ixgbevf_dev_stats_reset(eth_dev);

	/* Disable the interrupts for VF */
	ixgbevf_intr_disable(hw);

	hw->mac.num_rar_entries = 128; /* The MAX of the underlying PF */
	diag = hw->mac.ops.reset_hw(hw);

	/*
	 * The VF reset operation returns the IXGBE_ERR_INVALID_MAC_ADDR when
	 * the underlying PF driver has not assigned a MAC address to the VF.
	 * In this case, assign a random MAC address.
	 */
	if ((diag != IXGBE_SUCCESS) && (diag != IXGBE_ERR_INVALID_MAC_ADDR)) {
		PMD_INIT_LOG(ERR, "VF Initialization Failure: %d", diag);
		return diag;
	}

	/* negotiate mailbox API version to use with the PF. */
	ixgbevf_negotiate_api(hw);

	/* Get Rx/Tx queue count via mailbox, which is ready after reset_hw */
	ixgbevf_get_queues(hw, &tcs, &tc);

	/* Allocate memory for storing MAC addresses */
	eth_dev->data->mac_addrs = rte_zmalloc("ixgbevf", ETHER_ADDR_LEN *
			hw->mac.num_rar_entries, 0);
	if (eth_dev->data->mac_addrs == NULL) {
		PMD_INIT_LOG(ERR,
			"Failed to allocate %u bytes needed to store "
			"MAC addresses",
			ETHER_ADDR_LEN * hw->mac.num_rar_entries);
		return -ENOMEM;
	}

	/* Generate a random MAC address, if none was assigned by PF. */
	if (is_zero_ether_addr(perm_addr)) {
		generate_random_mac_addr(perm_addr);
		diag = ixgbe_set_rar_vf(hw, 1, perm_addr->addr_bytes, 0, 1);
		if (diag) {
			rte_free(eth_dev->data->mac_addrs);
			eth_dev->data->mac_addrs = NULL;
			return diag;
		}
		PMD_INIT_LOG(INFO, "\tVF MAC address not assigned by Host PF");
		PMD_INIT_LOG(INFO, "\tAssign randomly generated MAC address "
			     "%02x:%02x:%02x:%02x:%02x:%02x",
			     perm_addr->addr_bytes[0],
			     perm_addr->addr_bytes[1],
			     perm_addr->addr_bytes[2],
			     perm_addr->addr_bytes[3],
			     perm_addr->addr_bytes[4],
			     perm_addr->addr_bytes[5]);
	}

	/* Copy the permanent MAC address */
	ether_addr_copy(perm_addr, &eth_dev->data->mac_addrs[0]);

	/* reset the hardware with the new settings */
	diag = hw->mac.ops.start_hw(hw);
	switch (diag) {
		case  0:
			break;

		default:
			PMD_INIT_LOG(ERR, "VF Initialization Failure: %d", diag);
			return -EIO;
	}

	PMD_INIT_LOG(DEBUG, "port %d vendorID=0x%x deviceID=0x%x mac.type=%s",
		     eth_dev->data->port_id, pci_dev->id.vendor_id,
		     pci_dev->id.device_id, "ixgbe_mac_82599_vf");

	return 0;
}

/* Virtual Function device uninit */

static int
eth_ixgbevf_dev_uninit(struct rte_eth_dev *eth_dev)
{
	struct ixgbe_hw *hw;
	unsigned i;

	PMD_INIT_FUNC_TRACE();

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return -EPERM;

	hw = IXGBE_DEV_PRIVATE_TO_HW(eth_dev->data->dev_private);

	if (hw->adapter_stopped == 0)
		ixgbevf_dev_close(eth_dev);

	eth_dev->dev_ops = NULL;
	eth_dev->rx_pkt_burst = NULL;
	eth_dev->tx_pkt_burst = NULL;

	/* Disable the interrupts for VF */
	ixgbevf_intr_disable(hw);

	for (i = 0; i < eth_dev->data->nb_rx_queues; i++) {
		ixgbe_dev_rx_queue_release(eth_dev->data->rx_queues[i]);
		eth_dev->data->rx_queues[i] = NULL;
	}
	eth_dev->data->nb_rx_queues = 0;

	for (i = 0; i < eth_dev->data->nb_tx_queues; i++) {
		ixgbe_dev_tx_queue_release(eth_dev->data->tx_queues[i]);
		eth_dev->data->tx_queues[i] = NULL;
	}
	eth_dev->data->nb_tx_queues = 0;

	rte_free(eth_dev->data->mac_addrs);
	eth_dev->data->mac_addrs = NULL;

	return 0;
}

static struct eth_driver rte_ixgbe_pmd = {
	.pci_drv = {
		.name = "rte_ixgbe_pmd",
		.id_table = pci_id_ixgbe_map,
		.drv_flags = RTE_PCI_DRV_NEED_MAPPING | RTE_PCI_DRV_INTR_LSC |
			RTE_PCI_DRV_DETACHABLE,
	},
	.eth_dev_init = eth_ixgbe_dev_init,
	.eth_dev_uninit = eth_ixgbe_dev_uninit,
	.dev_private_size = sizeof(struct ixgbe_adapter),
};

/*
 * virtual function driver struct
 */
static struct eth_driver rte_ixgbevf_pmd = {
	.pci_drv = {
		.name = "rte_ixgbevf_pmd",
		.id_table = pci_id_ixgbevf_map,
		.drv_flags = RTE_PCI_DRV_NEED_MAPPING | RTE_PCI_DRV_DETACHABLE,
	},
	.eth_dev_init = eth_ixgbevf_dev_init,
	.eth_dev_uninit = eth_ixgbevf_dev_uninit,
	.dev_private_size = sizeof(struct ixgbe_adapter),
};

/*
 * Driver initialization routine.
 * Invoked once at EAL init time.
 * Register itself as the [Poll Mode] Driver of PCI IXGBE devices.
 */
static int
rte_ixgbe_pmd_init(const char *name __rte_unused, const char *params __rte_unused)
{
	PMD_INIT_FUNC_TRACE();

	rte_eth_driver_register(&rte_ixgbe_pmd);
	return 0;
}

/*
 * VF Driver initialization routine.
 * Invoked one at EAL init time.
 * Register itself as the [Virtual Poll Mode] Driver of PCI niantic devices.
 */
static int
rte_ixgbevf_pmd_init(const char *name __rte_unused, const char *param __rte_unused)
{
	PMD_INIT_FUNC_TRACE();

	rte_eth_driver_register(&rte_ixgbevf_pmd);
	return 0;
}

static int
ixgbe_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_vfta * shadow_vfta =
		IXGBE_DEV_PRIVATE_TO_VFTA(dev->data->dev_private);
	uint32_t vfta;
	uint32_t vid_idx;
	uint32_t vid_bit;

	vid_idx = (uint32_t) ((vlan_id >> 5) & 0x7F);
	vid_bit = (uint32_t) (1 << (vlan_id & 0x1F));
	vfta = IXGBE_READ_REG(hw, IXGBE_VFTA(vid_idx));
	if (on)
		vfta |= vid_bit;
	else
		vfta &= ~vid_bit;
	IXGBE_WRITE_REG(hw, IXGBE_VFTA(vid_idx), vfta);

	/* update local VFTA copy */
	shadow_vfta->vfta[vid_idx] = vfta;

	return 0;
}

static void
ixgbe_vlan_strip_queue_set(struct rte_eth_dev *dev, uint16_t queue, int on)
{
	if (on)
		ixgbe_vlan_hw_strip_enable(dev, queue);
	else
		ixgbe_vlan_hw_strip_disable(dev, queue);
}

static void
ixgbe_vlan_tpid_set(struct rte_eth_dev *dev, uint16_t tpid)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	/* Only the high 16-bits is valid */
	IXGBE_WRITE_REG(hw, IXGBE_EXVET, tpid << 16);
}

void
ixgbe_vlan_hw_filter_disable(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t vlnctrl;

	PMD_INIT_FUNC_TRACE();

	/* Filter Table Disable */
	vlnctrl = IXGBE_READ_REG(hw, IXGBE_VLNCTRL);
	vlnctrl &= ~IXGBE_VLNCTRL_VFE;

	IXGBE_WRITE_REG(hw, IXGBE_VLNCTRL, vlnctrl);
}

void
ixgbe_vlan_hw_filter_enable(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_vfta * shadow_vfta =
		IXGBE_DEV_PRIVATE_TO_VFTA(dev->data->dev_private);
	uint32_t vlnctrl;
	uint16_t i;

	PMD_INIT_FUNC_TRACE();

	/* Filter Table Enable */
	vlnctrl = IXGBE_READ_REG(hw, IXGBE_VLNCTRL);
	vlnctrl &= ~IXGBE_VLNCTRL_CFIEN;
	vlnctrl |= IXGBE_VLNCTRL_VFE;

	IXGBE_WRITE_REG(hw, IXGBE_VLNCTRL, vlnctrl);

	/* write whatever is in local vfta copy */
	for (i = 0; i < IXGBE_VFTA_SIZE; i++)
		IXGBE_WRITE_REG(hw, IXGBE_VFTA(i), shadow_vfta->vfta[i]);
}

static void
ixgbe_vlan_hw_strip_bitmap_set(struct rte_eth_dev *dev, uint16_t queue, bool on)
{
	struct ixgbe_hwstrip *hwstrip =
		IXGBE_DEV_PRIVATE_TO_HWSTRIP_BITMAP(dev->data->dev_private);

	if(queue >= IXGBE_MAX_RX_QUEUE_NUM)
		return;

	if (on)
		IXGBE_SET_HWSTRIP(hwstrip, queue);
	else
		IXGBE_CLEAR_HWSTRIP(hwstrip, queue);
}

static void
ixgbe_vlan_hw_strip_disable(struct rte_eth_dev *dev, uint16_t queue)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t ctrl;

	PMD_INIT_FUNC_TRACE();

	if (hw->mac.type == ixgbe_mac_82598EB) {
		/* No queue level support */
		PMD_INIT_LOG(NOTICE, "82598EB not support queue level hw strip");
		return;
	}
	else {
		/* Other 10G NIC, the VLAN strip can be setup per queue in RXDCTL */
		ctrl = IXGBE_READ_REG(hw, IXGBE_RXDCTL(queue));
		ctrl &= ~IXGBE_RXDCTL_VME;
		IXGBE_WRITE_REG(hw, IXGBE_RXDCTL(queue), ctrl);
	}
	/* record those setting for HW strip per queue */
	ixgbe_vlan_hw_strip_bitmap_set(dev, queue, 0);
}

static void
ixgbe_vlan_hw_strip_enable(struct rte_eth_dev *dev, uint16_t queue)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t ctrl;

	PMD_INIT_FUNC_TRACE();

	if (hw->mac.type == ixgbe_mac_82598EB) {
		/* No queue level supported */
		PMD_INIT_LOG(NOTICE, "82598EB not support queue level hw strip");
		return;
	}
	else {
		/* Other 10G NIC, the VLAN strip can be setup per queue in RXDCTL */
		ctrl = IXGBE_READ_REG(hw, IXGBE_RXDCTL(queue));
		ctrl |= IXGBE_RXDCTL_VME;
		IXGBE_WRITE_REG(hw, IXGBE_RXDCTL(queue), ctrl);
	}
	/* record those setting for HW strip per queue */
	ixgbe_vlan_hw_strip_bitmap_set(dev, queue, 1);
}

void
ixgbe_vlan_hw_strip_disable_all(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t ctrl;
	uint16_t i;

	PMD_INIT_FUNC_TRACE();

	if (hw->mac.type == ixgbe_mac_82598EB) {
		ctrl = IXGBE_READ_REG(hw, IXGBE_VLNCTRL);
		ctrl &= ~IXGBE_VLNCTRL_VME;
		IXGBE_WRITE_REG(hw, IXGBE_VLNCTRL, ctrl);
	}
	else {
		/* Other 10G NIC, the VLAN strip can be setup per queue in RXDCTL */
		for (i = 0; i < dev->data->nb_rx_queues; i++) {
			ctrl = IXGBE_READ_REG(hw, IXGBE_RXDCTL(i));
			ctrl &= ~IXGBE_RXDCTL_VME;
			IXGBE_WRITE_REG(hw, IXGBE_RXDCTL(i), ctrl);

			/* record those setting for HW strip per queue */
			ixgbe_vlan_hw_strip_bitmap_set(dev, i, 0);
		}
	}
}

void
ixgbe_vlan_hw_strip_enable_all(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t ctrl;
	uint16_t i;

	PMD_INIT_FUNC_TRACE();

	if (hw->mac.type == ixgbe_mac_82598EB) {
		ctrl = IXGBE_READ_REG(hw, IXGBE_VLNCTRL);
		ctrl |= IXGBE_VLNCTRL_VME;
		IXGBE_WRITE_REG(hw, IXGBE_VLNCTRL, ctrl);
	}
	else {
		/* Other 10G NIC, the VLAN strip can be setup per queue in RXDCTL */
		for (i = 0; i < dev->data->nb_rx_queues; i++) {
			ctrl = IXGBE_READ_REG(hw, IXGBE_RXDCTL(i));
			ctrl |= IXGBE_RXDCTL_VME;
			IXGBE_WRITE_REG(hw, IXGBE_RXDCTL(i), ctrl);

			/* record those setting for HW strip per queue */
			ixgbe_vlan_hw_strip_bitmap_set(dev, i, 1);
		}
	}
}

static void
ixgbe_vlan_hw_extend_disable(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t ctrl;

	PMD_INIT_FUNC_TRACE();

	/* DMATXCTRL: Geric Double VLAN Disable */
	ctrl = IXGBE_READ_REG(hw, IXGBE_DMATXCTL);
	ctrl &= ~IXGBE_DMATXCTL_GDV;
	IXGBE_WRITE_REG(hw, IXGBE_DMATXCTL, ctrl);

	/* CTRL_EXT: Global Double VLAN Disable */
	ctrl = IXGBE_READ_REG(hw, IXGBE_CTRL_EXT);
	ctrl &= ~IXGBE_EXTENDED_VLAN;
	IXGBE_WRITE_REG(hw, IXGBE_CTRL_EXT, ctrl);

}

static void
ixgbe_vlan_hw_extend_enable(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t ctrl;

	PMD_INIT_FUNC_TRACE();

	/* DMATXCTRL: Geric Double VLAN Enable */
	ctrl  = IXGBE_READ_REG(hw, IXGBE_DMATXCTL);
	ctrl |= IXGBE_DMATXCTL_GDV;
	IXGBE_WRITE_REG(hw, IXGBE_DMATXCTL, ctrl);

	/* CTRL_EXT: Global Double VLAN Enable */
	ctrl  = IXGBE_READ_REG(hw, IXGBE_CTRL_EXT);
	ctrl |= IXGBE_EXTENDED_VLAN;
	IXGBE_WRITE_REG(hw, IXGBE_CTRL_EXT, ctrl);

	/*
	 * VET EXT field in the EXVET register = 0x8100 by default
	 * So no need to change. Same to VT field of DMATXCTL register
	 */
}

static void
ixgbe_vlan_offload_set(struct rte_eth_dev *dev, int mask)
{
	if(mask & ETH_VLAN_STRIP_MASK){
		if (dev->data->dev_conf.rxmode.hw_vlan_strip)
			ixgbe_vlan_hw_strip_enable_all(dev);
		else
			ixgbe_vlan_hw_strip_disable_all(dev);
	}

	if(mask & ETH_VLAN_FILTER_MASK){
		if (dev->data->dev_conf.rxmode.hw_vlan_filter)
			ixgbe_vlan_hw_filter_enable(dev);
		else
			ixgbe_vlan_hw_filter_disable(dev);
	}

	if(mask & ETH_VLAN_EXTEND_MASK){
		if (dev->data->dev_conf.rxmode.hw_vlan_extend)
			ixgbe_vlan_hw_extend_enable(dev);
		else
			ixgbe_vlan_hw_extend_disable(dev);
	}
}

static void
ixgbe_vmdq_vlan_hw_filter_enable(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	/* VLNCTRL: enable vlan filtering and allow all vlan tags through */
	uint32_t vlanctrl = IXGBE_READ_REG(hw, IXGBE_VLNCTRL);
	vlanctrl |= IXGBE_VLNCTRL_VFE ; /* enable vlan filters */
	IXGBE_WRITE_REG(hw, IXGBE_VLNCTRL, vlanctrl);
}

static int
ixgbe_check_vf_rss_rxq_num(struct rte_eth_dev *dev, uint16_t nb_rx_q)
{
	switch (nb_rx_q) {
	case 1:
	case 2:
		RTE_ETH_DEV_SRIOV(dev).active = ETH_64_POOLS;
		break;
	case 4:
		RTE_ETH_DEV_SRIOV(dev).active = ETH_32_POOLS;
		break;
	default:
		return -EINVAL;
	}

	RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool = nb_rx_q;
	RTE_ETH_DEV_SRIOV(dev).def_pool_q_idx = dev->pci_dev->max_vfs * nb_rx_q;

	return 0;
}

static int
ixgbe_check_mq_mode(struct rte_eth_dev *dev)
{
	struct rte_eth_conf *dev_conf = &dev->data->dev_conf;
	uint16_t nb_rx_q = dev->data->nb_rx_queues;
	uint16_t nb_tx_q = dev->data->nb_rx_queues;

	if (RTE_ETH_DEV_SRIOV(dev).active != 0) {
		/* check multi-queue mode */
		switch (dev_conf->rxmode.mq_mode) {
		case ETH_MQ_RX_VMDQ_DCB:
		case ETH_MQ_RX_VMDQ_DCB_RSS:
			/* DCB/RSS VMDQ in SRIOV mode, not implement yet */
			PMD_INIT_LOG(ERR, "SRIOV active,"
					" unsupported mq_mode rx %d.",
					dev_conf->rxmode.mq_mode);
			return -EINVAL;
		case ETH_MQ_RX_RSS:
		case ETH_MQ_RX_VMDQ_RSS:
			dev->data->dev_conf.rxmode.mq_mode = ETH_MQ_RX_VMDQ_RSS;
			if (nb_rx_q <= RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool)
				if (ixgbe_check_vf_rss_rxq_num(dev, nb_rx_q)) {
					PMD_INIT_LOG(ERR, "SRIOV is active,"
						" invalid queue number"
						" for VMDQ RSS, allowed"
						" value are 1, 2 or 4.");
					return -EINVAL;
				}
			break;
		case ETH_MQ_RX_VMDQ_ONLY:
		case ETH_MQ_RX_NONE:
			/* if nothing mq mode configure, use default scheme */
			dev->data->dev_conf.rxmode.mq_mode = ETH_MQ_RX_VMDQ_ONLY;
			if (RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool > 1)
				RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool = 1;
			break;
		default: /* ETH_MQ_RX_DCB, ETH_MQ_RX_DCB_RSS or ETH_MQ_TX_DCB*/
			/* SRIOV only works in VMDq enable mode */
			PMD_INIT_LOG(ERR, "SRIOV is active,"
					" wrong mq_mode rx %d.",
					dev_conf->rxmode.mq_mode);
			return -EINVAL;
		}

		switch (dev_conf->txmode.mq_mode) {
		case ETH_MQ_TX_VMDQ_DCB:
			/* DCB VMDQ in SRIOV mode, not implement yet */
			PMD_INIT_LOG(ERR, "SRIOV is active,"
					" unsupported VMDQ mq_mode tx %d.",
					dev_conf->txmode.mq_mode);
			return -EINVAL;
		default: /* ETH_MQ_TX_VMDQ_ONLY or ETH_MQ_TX_NONE */
			dev->data->dev_conf.txmode.mq_mode = ETH_MQ_TX_VMDQ_ONLY;
			break;
		}

		/* check valid queue number */
		if ((nb_rx_q > RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool) ||
		    (nb_tx_q > RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool)) {
			PMD_INIT_LOG(ERR, "SRIOV is active,"
					" queue number must less equal to %d.",
					RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool);
			return -EINVAL;
		}
	} else {
		/* check configuration for vmdb+dcb mode */
		if (dev_conf->rxmode.mq_mode == ETH_MQ_RX_VMDQ_DCB) {
			const struct rte_eth_vmdq_dcb_conf *conf;

			if (nb_rx_q != IXGBE_VMDQ_DCB_NB_QUEUES) {
				PMD_INIT_LOG(ERR, "VMDQ+DCB, nb_rx_q != %d.",
						IXGBE_VMDQ_DCB_NB_QUEUES);
				return -EINVAL;
			}
			conf = &dev_conf->rx_adv_conf.vmdq_dcb_conf;
			if (!(conf->nb_queue_pools == ETH_16_POOLS ||
			       conf->nb_queue_pools == ETH_32_POOLS)) {
				PMD_INIT_LOG(ERR, "VMDQ+DCB selected,"
						" nb_queue_pools must be %d or %d.",
						ETH_16_POOLS, ETH_32_POOLS);
				return -EINVAL;
			}
		}
		if (dev_conf->txmode.mq_mode == ETH_MQ_TX_VMDQ_DCB) {
			const struct rte_eth_vmdq_dcb_tx_conf *conf;

			if (nb_tx_q != IXGBE_VMDQ_DCB_NB_QUEUES) {
				PMD_INIT_LOG(ERR, "VMDQ+DCB, nb_tx_q != %d",
						 IXGBE_VMDQ_DCB_NB_QUEUES);
				return -EINVAL;
			}
			conf = &dev_conf->tx_adv_conf.vmdq_dcb_tx_conf;
			if (!(conf->nb_queue_pools == ETH_16_POOLS ||
			       conf->nb_queue_pools == ETH_32_POOLS)) {
				PMD_INIT_LOG(ERR, "VMDQ+DCB selected,"
						" nb_queue_pools != %d and"
						" nb_queue_pools != %d.",
						ETH_16_POOLS, ETH_32_POOLS);
				return -EINVAL;
			}
		}

		/* For DCB mode check our configuration before we go further */
		if (dev_conf->rxmode.mq_mode == ETH_MQ_RX_DCB) {
			const struct rte_eth_dcb_rx_conf *conf;

			if (nb_rx_q != IXGBE_DCB_NB_QUEUES) {
				PMD_INIT_LOG(ERR, "DCB selected, nb_rx_q != %d.",
						 IXGBE_DCB_NB_QUEUES);
				return -EINVAL;
			}
			conf = &dev_conf->rx_adv_conf.dcb_rx_conf;
			if (!(conf->nb_tcs == ETH_4_TCS ||
			       conf->nb_tcs == ETH_8_TCS)) {
				PMD_INIT_LOG(ERR, "DCB selected, nb_tcs != %d"
						" and nb_tcs != %d.",
						ETH_4_TCS, ETH_8_TCS);
				return -EINVAL;
			}
		}

		if (dev_conf->txmode.mq_mode == ETH_MQ_TX_DCB) {
			const struct rte_eth_dcb_tx_conf *conf;

			if (nb_tx_q != IXGBE_DCB_NB_QUEUES) {
				PMD_INIT_LOG(ERR, "DCB, nb_tx_q != %d.",
						 IXGBE_DCB_NB_QUEUES);
				return -EINVAL;
			}
			conf = &dev_conf->tx_adv_conf.dcb_tx_conf;
			if (!(conf->nb_tcs == ETH_4_TCS ||
			       conf->nb_tcs == ETH_8_TCS)) {
				PMD_INIT_LOG(ERR, "DCB selected, nb_tcs != %d"
						" and nb_tcs != %d.",
						ETH_4_TCS, ETH_8_TCS);
				return -EINVAL;
			}
		}
	}
	return 0;
}

static int
ixgbe_dev_configure(struct rte_eth_dev *dev)
{
	struct ixgbe_interrupt *intr =
		IXGBE_DEV_PRIVATE_TO_INTR(dev->data->dev_private);
	struct ixgbe_adapter *adapter =
		(struct ixgbe_adapter *)dev->data->dev_private;
	int ret;

	PMD_INIT_FUNC_TRACE();
	/* multipe queue mode checking */
	ret  = ixgbe_check_mq_mode(dev);
	if (ret != 0) {
		PMD_DRV_LOG(ERR, "ixgbe_check_mq_mode fails with %d.",
			    ret);
		return ret;
	}

	/* set flag to update link status after init */
	intr->flags |= IXGBE_FLAG_NEED_LINK_UPDATE;

	/*
	 * Initialize to TRUE. If any of Rx queues doesn't meet the bulk
	 * allocation or vector Rx preconditions we will reset it.
	 */
	adapter->rx_bulk_alloc_allowed = true;
	adapter->rx_vec_allowed = true;

	return 0;
}

/*
 * Configure device link speed and setup link.
 * It returns 0 on success.
 */
static int
ixgbe_dev_start(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_vf_info *vfinfo =
		*IXGBE_DEV_PRIVATE_TO_P_VFDATA(dev->data->dev_private);
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;
	uint32_t intr_vector = 0;
	int err, link_up = 0, negotiate = 0;
	uint32_t speed = 0;
	int mask = 0;
	int status;
	uint16_t vf, idx;

	PMD_INIT_FUNC_TRACE();

	/* IXGBE devices don't support half duplex */
	if ((dev->data->dev_conf.link_duplex != ETH_LINK_AUTONEG_DUPLEX) &&
			(dev->data->dev_conf.link_duplex != ETH_LINK_FULL_DUPLEX)) {
		PMD_INIT_LOG(ERR, "Invalid link_duplex (%hu) for port %hhu",
			     dev->data->dev_conf.link_duplex,
			     dev->data->port_id);
		return -EINVAL;
	}

	/* disable uio/vfio intr/eventfd mapping */
	rte_intr_disable(intr_handle);

	/* stop adapter */
	hw->adapter_stopped = 0;
	ixgbe_stop_adapter(hw);

	/* reinitialize adapter
	 * this calls reset and start */
	status = ixgbe_pf_reset_hw(hw);
	if (status != 0)
		return -1;
	hw->mac.ops.start_hw(hw);
	hw->mac.get_link_status = true;

	/* configure PF module if SRIOV enabled */
	ixgbe_pf_host_configure(dev);

	/* check and configure queue intr-vector mapping */
	if ((rte_intr_cap_multiple(intr_handle) ||
	     !RTE_ETH_DEV_SRIOV(dev).active) &&
	    dev->data->dev_conf.intr_conf.rxq != 0) {
		intr_vector = dev->data->nb_rx_queues;
		if (rte_intr_efd_enable(intr_handle, intr_vector))
			return -1;
	}

	if (rte_intr_dp_is_en(intr_handle) && !intr_handle->intr_vec) {
		intr_handle->intr_vec =
			rte_zmalloc("intr_vec",
				    dev->data->nb_rx_queues * sizeof(int), 0);
		if (intr_handle->intr_vec == NULL) {
			PMD_INIT_LOG(ERR, "Failed to allocate %d rx_queues"
				     " intr_vec\n", dev->data->nb_rx_queues);
			return -ENOMEM;
		}
	}

	/* confiugre msix for sleep until rx interrupt */
	ixgbe_configure_msix(dev);

	/* initialize transmission unit */
	ixgbe_dev_tx_init(dev);

	/* This can fail when allocating mbufs for descriptor rings */
	err = ixgbe_dev_rx_init(dev);
	if (err) {
		PMD_INIT_LOG(ERR, "Unable to initialize RX hardware");
		goto error;
	}

	err = ixgbe_dev_rxtx_start(dev);
	if (err < 0) {
		PMD_INIT_LOG(ERR, "Unable to start rxtx queues");
		goto error;
	}

	/* Skip link setup if loopback mode is enabled for 82599. */
	if (hw->mac.type == ixgbe_mac_82599EB &&
			dev->data->dev_conf.lpbk_mode == IXGBE_LPBK_82599_TX_RX)
		goto skip_link_setup;

	if (ixgbe_is_sfp(hw) && hw->phy.multispeed_fiber) {
		err = hw->mac.ops.setup_sfp(hw);
		if (err)
			goto error;
	}

	if (hw->mac.ops.get_media_type(hw) == ixgbe_media_type_copper) {
		/* Turn on the copper */
		ixgbe_set_phy_power(hw, true);
	} else {
		/* Turn on the laser */
		ixgbe_enable_tx_laser(hw);
	}

	err = ixgbe_check_link(hw, &speed, &link_up, 0);
	if (err)
		goto error;
	dev->data->dev_link.link_status = link_up;

	err = ixgbe_get_link_capabilities(hw, &speed, &negotiate);
	if (err)
		goto error;

	switch(dev->data->dev_conf.link_speed) {
	case ETH_LINK_SPEED_AUTONEG:
		speed = (hw->mac.type != ixgbe_mac_82598EB) ?
				IXGBE_LINK_SPEED_82599_AUTONEG :
				IXGBE_LINK_SPEED_82598_AUTONEG;
		break;
	case ETH_LINK_SPEED_100:
		/*
		 * Invalid for 82598 but error will be detected by
		 * ixgbe_setup_link()
		 */
		speed = IXGBE_LINK_SPEED_100_FULL;
		break;
	case ETH_LINK_SPEED_1000:
		speed = IXGBE_LINK_SPEED_1GB_FULL;
		break;
	case ETH_LINK_SPEED_10000:
		speed = IXGBE_LINK_SPEED_10GB_FULL;
		break;
	default:
		PMD_INIT_LOG(ERR, "Invalid link_speed (%hu) for port %hhu",
			     dev->data->dev_conf.link_speed,
			     dev->data->port_id);
		goto error;
	}

	err = ixgbe_setup_link(hw, speed, link_up);
	if (err)
		goto error;

skip_link_setup:

	if (rte_intr_allow_others(intr_handle)) {
		/* check if lsc interrupt is enabled */
		if (dev->data->dev_conf.intr_conf.lsc != 0)
			ixgbe_dev_lsc_interrupt_setup(dev);
	} else {
		rte_intr_callback_unregister(intr_handle,
					     ixgbe_dev_interrupt_handler,
					     (void *)dev);
		if (dev->data->dev_conf.intr_conf.lsc != 0)
			PMD_INIT_LOG(INFO, "lsc won't enable because of"
				     " no intr multiplex\n");
	}

	/* check if rxq interrupt is enabled */
	if (dev->data->dev_conf.intr_conf.rxq != 0 &&
	    rte_intr_dp_is_en(intr_handle))
		ixgbe_dev_rxq_interrupt_setup(dev);

	/* enable uio/vfio intr/eventfd mapping */
	rte_intr_enable(intr_handle);

	/* resume enabled intr since hw reset */
	ixgbe_enable_intr(dev);

	mask = ETH_VLAN_STRIP_MASK | ETH_VLAN_FILTER_MASK | \
		ETH_VLAN_EXTEND_MASK;
	ixgbe_vlan_offload_set(dev, mask);

	if (dev->data->dev_conf.rxmode.mq_mode == ETH_MQ_RX_VMDQ_ONLY) {
		/* Enable vlan filtering for VMDq */
		ixgbe_vmdq_vlan_hw_filter_enable(dev);
	}

	/* Configure DCB hw */
	ixgbe_configure_dcb(dev);

	if (dev->data->dev_conf.fdir_conf.mode != RTE_FDIR_MODE_NONE) {
		err = ixgbe_fdir_configure(dev);
		if (err)
			goto error;
	}

	/* Restore vf rate limit */
	if (vfinfo != NULL) {
		for (vf = 0; vf < dev->pci_dev->max_vfs; vf++)
			for (idx = 0; idx < IXGBE_MAX_QUEUE_NUM_PER_VF; idx++)
				if (vfinfo[vf].tx_rate[idx] != 0)
					ixgbe_set_vf_rate_limit(dev, vf,
						vfinfo[vf].tx_rate[idx],
						1 << idx);
	}

	ixgbe_restore_statistics_mapping(dev);

	return 0;

error:
	PMD_INIT_LOG(ERR, "failure in ixgbe_dev_start(): %d", err);
	ixgbe_dev_clear_queues(dev);
	return -EIO;
}

/*
 * Stop device: disable rx and tx functions to allow for reconfiguring.
 */
static void
ixgbe_dev_stop(struct rte_eth_dev *dev)
{
	struct rte_eth_link link;
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_vf_info *vfinfo =
		*IXGBE_DEV_PRIVATE_TO_P_VFDATA(dev->data->dev_private);
	struct ixgbe_filter_info *filter_info =
		IXGBE_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	struct ixgbe_5tuple_filter *p_5tuple, *p_5tuple_next;
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;
	int vf;

	PMD_INIT_FUNC_TRACE();

	/* disable interrupts */
	ixgbe_disable_intr(hw);

	/* disable intr eventfd mapping */
	rte_intr_disable(intr_handle);

	/* reset the NIC */
	ixgbe_pf_reset_hw(hw);
	hw->adapter_stopped = 0;

	/* stop adapter */
	ixgbe_stop_adapter(hw);

	for (vf = 0; vfinfo != NULL &&
		     vf < dev->pci_dev->max_vfs; vf++)
		vfinfo[vf].clear_to_send = false;

	if (hw->mac.ops.get_media_type(hw) == ixgbe_media_type_copper) {
		/* Turn off the copper */
		ixgbe_set_phy_power(hw, false);
	} else {
		/* Turn off the laser */
		ixgbe_disable_tx_laser(hw);
	}

	ixgbe_dev_clear_queues(dev);

	/* Clear stored conf */
	dev->data->scattered_rx = 0;
	dev->data->lro = 0;

	/* Clear recorded link status */
	memset(&link, 0, sizeof(link));
	rte_ixgbe_dev_atomic_write_link_status(dev, &link);

	/* Remove all ntuple filters of the device */
	for (p_5tuple = TAILQ_FIRST(&filter_info->fivetuple_list);
	     p_5tuple != NULL; p_5tuple = p_5tuple_next) {
		p_5tuple_next = TAILQ_NEXT(p_5tuple, entries);
		TAILQ_REMOVE(&filter_info->fivetuple_list,
			     p_5tuple, entries);
		rte_free(p_5tuple);
	}
	memset(filter_info->fivetuple_mask, 0,
		sizeof(uint32_t) * IXGBE_5TUPLE_ARRAY_SIZE);

	if (!rte_intr_allow_others(intr_handle))
		/* resume to the default handler */
		rte_intr_callback_register(intr_handle,
					   ixgbe_dev_interrupt_handler,
					   (void *)dev);

	/* Clean datapath event and queue/vec mapping */
	rte_intr_efd_disable(intr_handle);
	if (intr_handle->intr_vec != NULL) {
		rte_free(intr_handle->intr_vec);
		intr_handle->intr_vec = NULL;
	}
}

/*
 * Set device link up: enable tx.
 */
static int
ixgbe_dev_set_link_up(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	if (hw->mac.type == ixgbe_mac_82599EB) {
#ifdef RTE_NIC_BYPASS
		if (hw->device_id == IXGBE_DEV_ID_82599_BYPASS) {
			/* Not suported in bypass mode */
			PMD_INIT_LOG(ERR, "Set link up is not supported "
				     "by device id 0x%x", hw->device_id);
			return -ENOTSUP;
		}
#endif
	}

	if (hw->mac.ops.get_media_type(hw) == ixgbe_media_type_copper) {
		/* Turn on the copper */
		ixgbe_set_phy_power(hw, true);
	} else {
		/* Turn on the laser */
		ixgbe_enable_tx_laser(hw);
	}

	return 0;
}

/*
 * Set device link down: disable tx.
 */
static int
ixgbe_dev_set_link_down(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	if (hw->mac.type == ixgbe_mac_82599EB) {
#ifdef RTE_NIC_BYPASS
		if (hw->device_id == IXGBE_DEV_ID_82599_BYPASS) {
			/* Not suported in bypass mode */
			PMD_INIT_LOG(ERR, "Set link down is not supported "
				     "by device id 0x%x", hw->device_id);
			return -ENOTSUP;
		}
#endif
	}

	if (hw->mac.ops.get_media_type(hw) == ixgbe_media_type_copper) {
		/* Turn off the copper */
		ixgbe_set_phy_power(hw, false);
	} else {
		/* Turn off the laser */
		ixgbe_disable_tx_laser(hw);
	}

	return 0;
}

/*
 * Reest and stop device.
 */
static void
ixgbe_dev_close(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	PMD_INIT_FUNC_TRACE();

	ixgbe_pf_reset_hw(hw);

	ixgbe_dev_stop(dev);
	hw->adapter_stopped = 1;

	ixgbe_dev_free_queues(dev);

	ixgbe_disable_pcie_master(hw);

	/* reprogram the RAR[0] in case user changed it. */
	ixgbe_set_rar(hw, 0, hw->mac.addr, 0, IXGBE_RAH_AV);
}

static void
ixgbe_read_stats_registers(struct ixgbe_hw *hw,
			   struct ixgbe_hw_stats *hw_stats,
			   uint64_t *total_missed_rx, uint64_t *total_qbrc,
			   uint64_t *total_qprc, uint64_t *total_qprdc)
{
	uint32_t bprc, lxon, lxoff, total;
	uint32_t delta_gprc = 0;
	unsigned i;
	/* Workaround for RX byte count not including CRC bytes when CRC
+	 * strip is enabled. CRC bytes are removed from counters when crc_strip
	 * is disabled.
+	 */
	int crc_strip = (IXGBE_READ_REG(hw, IXGBE_HLREG0) &
			IXGBE_HLREG0_RXCRCSTRP);

	hw_stats->crcerrs += IXGBE_READ_REG(hw, IXGBE_CRCERRS);
	hw_stats->illerrc += IXGBE_READ_REG(hw, IXGBE_ILLERRC);
	hw_stats->errbc += IXGBE_READ_REG(hw, IXGBE_ERRBC);
	hw_stats->mspdc += IXGBE_READ_REG(hw, IXGBE_MSPDC);

	for (i = 0; i < 8; i++) {
		uint32_t mp;
		mp = IXGBE_READ_REG(hw, IXGBE_MPC(i));
		/* global total per queue */
		hw_stats->mpc[i] += mp;
		/* Running comprehensive total for stats display */
		*total_missed_rx += hw_stats->mpc[i];
		if (hw->mac.type == ixgbe_mac_82598EB) {
			hw_stats->rnbc[i] +=
			    IXGBE_READ_REG(hw, IXGBE_RNBC(i));
			hw_stats->pxonrxc[i] +=
				IXGBE_READ_REG(hw, IXGBE_PXONRXC(i));
			hw_stats->pxoffrxc[i] +=
				IXGBE_READ_REG(hw, IXGBE_PXOFFRXC(i));
		} else {
			hw_stats->pxonrxc[i] +=
				IXGBE_READ_REG(hw, IXGBE_PXONRXCNT(i));
			hw_stats->pxoffrxc[i] +=
				IXGBE_READ_REG(hw, IXGBE_PXOFFRXCNT(i));
			hw_stats->pxon2offc[i] +=
				IXGBE_READ_REG(hw, IXGBE_PXON2OFFCNT(i));
		}
		hw_stats->pxontxc[i] +=
		    IXGBE_READ_REG(hw, IXGBE_PXONTXC(i));
		hw_stats->pxofftxc[i] +=
		    IXGBE_READ_REG(hw, IXGBE_PXOFFTXC(i));
	}
	for (i = 0; i < IXGBE_QUEUE_STAT_COUNTERS; i++) {
		uint32_t delta_qprc = IXGBE_READ_REG(hw, IXGBE_QPRC(i));
		uint32_t delta_qptc = IXGBE_READ_REG(hw, IXGBE_QPTC(i));
		uint32_t delta_qprdc = IXGBE_READ_REG(hw, IXGBE_QPRDC(i));

		delta_gprc += delta_qprc;

		hw_stats->qprc[i] += delta_qprc;
		hw_stats->qptc[i] += delta_qptc;

		hw_stats->qbrc[i] += IXGBE_READ_REG(hw, IXGBE_QBRC_L(i));
		hw_stats->qbrc[i] +=
		    ((uint64_t)IXGBE_READ_REG(hw, IXGBE_QBRC_H(i)) << 32);
		if (crc_strip == 0)
			hw_stats->qbrc[i] -= delta_qprc * ETHER_CRC_LEN;

		hw_stats->qbtc[i] += IXGBE_READ_REG(hw, IXGBE_QBTC_L(i));
		hw_stats->qbtc[i] +=
		    ((uint64_t)IXGBE_READ_REG(hw, IXGBE_QBTC_H(i)) << 32);

		hw_stats->qprdc[i] += delta_qprdc;
		*total_qprdc += hw_stats->qprdc[i];

		*total_qprc += hw_stats->qprc[i];
		*total_qbrc += hw_stats->qbrc[i];
	}
	hw_stats->mlfc += IXGBE_READ_REG(hw, IXGBE_MLFC);
	hw_stats->mrfc += IXGBE_READ_REG(hw, IXGBE_MRFC);
	hw_stats->rlec += IXGBE_READ_REG(hw, IXGBE_RLEC);

	/*
	 * An errata states that gprc actually counts good + missed packets:
	 * Workaround to set gprc to summated queue packet receives
	 */
	hw_stats->gprc = *total_qprc;

	if (hw->mac.type != ixgbe_mac_82598EB) {
		hw_stats->gorc += IXGBE_READ_REG(hw, IXGBE_GORCL);
		hw_stats->gorc += ((u64)IXGBE_READ_REG(hw, IXGBE_GORCH) << 32);
		hw_stats->gotc += IXGBE_READ_REG(hw, IXGBE_GOTCL);
		hw_stats->gotc += ((u64)IXGBE_READ_REG(hw, IXGBE_GOTCH) << 32);
		hw_stats->tor += IXGBE_READ_REG(hw, IXGBE_TORL);
		hw_stats->tor += ((u64)IXGBE_READ_REG(hw, IXGBE_TORH) << 32);
		hw_stats->lxonrxc += IXGBE_READ_REG(hw, IXGBE_LXONRXCNT);
		hw_stats->lxoffrxc += IXGBE_READ_REG(hw, IXGBE_LXOFFRXCNT);
	} else {
		hw_stats->lxonrxc += IXGBE_READ_REG(hw, IXGBE_LXONRXC);
		hw_stats->lxoffrxc += IXGBE_READ_REG(hw, IXGBE_LXOFFRXC);
		/* 82598 only has a counter in the high register */
		hw_stats->gorc += IXGBE_READ_REG(hw, IXGBE_GORCH);
		hw_stats->gotc += IXGBE_READ_REG(hw, IXGBE_GOTCH);
		hw_stats->tor += IXGBE_READ_REG(hw, IXGBE_TORH);
	}
	uint64_t old_tpr = hw_stats->tpr;

	hw_stats->tpr += IXGBE_READ_REG(hw, IXGBE_TPR);
	hw_stats->tpt += IXGBE_READ_REG(hw, IXGBE_TPT);

	if (crc_strip == 0)
		hw_stats->gorc -= delta_gprc * ETHER_CRC_LEN;

	uint64_t delta_gptc = IXGBE_READ_REG(hw, IXGBE_GPTC);
	hw_stats->gptc += delta_gptc;
	hw_stats->gotc -= delta_gptc * ETHER_CRC_LEN;
	hw_stats->tor -= (hw_stats->tpr - old_tpr) * ETHER_CRC_LEN;

	/*
	 * Workaround: mprc hardware is incorrectly counting
	 * broadcasts, so for now we subtract those.
	 */
	bprc = IXGBE_READ_REG(hw, IXGBE_BPRC);
	hw_stats->bprc += bprc;
	hw_stats->mprc += IXGBE_READ_REG(hw, IXGBE_MPRC);
	if (hw->mac.type == ixgbe_mac_82598EB)
		hw_stats->mprc -= bprc;

	hw_stats->prc64 += IXGBE_READ_REG(hw, IXGBE_PRC64);
	hw_stats->prc127 += IXGBE_READ_REG(hw, IXGBE_PRC127);
	hw_stats->prc255 += IXGBE_READ_REG(hw, IXGBE_PRC255);
	hw_stats->prc511 += IXGBE_READ_REG(hw, IXGBE_PRC511);
	hw_stats->prc1023 += IXGBE_READ_REG(hw, IXGBE_PRC1023);
	hw_stats->prc1522 += IXGBE_READ_REG(hw, IXGBE_PRC1522);

	lxon = IXGBE_READ_REG(hw, IXGBE_LXONTXC);
	hw_stats->lxontxc += lxon;
	lxoff = IXGBE_READ_REG(hw, IXGBE_LXOFFTXC);
	hw_stats->lxofftxc += lxoff;
	total = lxon + lxoff;

	hw_stats->mptc += IXGBE_READ_REG(hw, IXGBE_MPTC);
	hw_stats->ptc64 += IXGBE_READ_REG(hw, IXGBE_PTC64);
	hw_stats->gptc -= total;
	hw_stats->mptc -= total;
	hw_stats->ptc64 -= total;
	hw_stats->gotc -= total * ETHER_MIN_LEN;

	hw_stats->ruc += IXGBE_READ_REG(hw, IXGBE_RUC);
	hw_stats->rfc += IXGBE_READ_REG(hw, IXGBE_RFC);
	hw_stats->roc += IXGBE_READ_REG(hw, IXGBE_ROC);
	hw_stats->rjc += IXGBE_READ_REG(hw, IXGBE_RJC);
	hw_stats->mngprc += IXGBE_READ_REG(hw, IXGBE_MNGPRC);
	hw_stats->mngpdc += IXGBE_READ_REG(hw, IXGBE_MNGPDC);
	hw_stats->mngptc += IXGBE_READ_REG(hw, IXGBE_MNGPTC);
	hw_stats->ptc127 += IXGBE_READ_REG(hw, IXGBE_PTC127);
	hw_stats->ptc255 += IXGBE_READ_REG(hw, IXGBE_PTC255);
	hw_stats->ptc511 += IXGBE_READ_REG(hw, IXGBE_PTC511);
	hw_stats->ptc1023 += IXGBE_READ_REG(hw, IXGBE_PTC1023);
	hw_stats->ptc1522 += IXGBE_READ_REG(hw, IXGBE_PTC1522);
	hw_stats->bptc += IXGBE_READ_REG(hw, IXGBE_BPTC);
	hw_stats->xec += IXGBE_READ_REG(hw, IXGBE_XEC);
	hw_stats->fccrc += IXGBE_READ_REG(hw, IXGBE_FCCRC);
	hw_stats->fclast += IXGBE_READ_REG(hw, IXGBE_FCLAST);
	/* Only read FCOE on 82599 */
	if (hw->mac.type != ixgbe_mac_82598EB) {
		hw_stats->fcoerpdc += IXGBE_READ_REG(hw, IXGBE_FCOERPDC);
		hw_stats->fcoeprc += IXGBE_READ_REG(hw, IXGBE_FCOEPRC);
		hw_stats->fcoeptc += IXGBE_READ_REG(hw, IXGBE_FCOEPTC);
		hw_stats->fcoedwrc += IXGBE_READ_REG(hw, IXGBE_FCOEDWRC);
		hw_stats->fcoedwtc += IXGBE_READ_REG(hw, IXGBE_FCOEDWTC);
	}

	/* Flow Director Stats registers */
	hw_stats->fdirmatch += IXGBE_READ_REG(hw, IXGBE_FDIRMATCH);
	hw_stats->fdirmiss += IXGBE_READ_REG(hw, IXGBE_FDIRMISS);
}

/*
 * This function is based on ixgbe_update_stats_counters() in ixgbe/ixgbe.c
 */
static void
ixgbe_dev_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	struct ixgbe_hw *hw =
			IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_hw_stats *hw_stats =
			IXGBE_DEV_PRIVATE_TO_STATS(dev->data->dev_private);
	uint64_t total_missed_rx, total_qbrc, total_qprc, total_qprdc;
	unsigned i;

	total_missed_rx = 0;
	total_qbrc = 0;
	total_qprc = 0;
	total_qprdc = 0;

	ixgbe_read_stats_registers(hw, hw_stats, &total_missed_rx, &total_qbrc,
			&total_qprc, &total_qprdc);

	if (stats == NULL)
		return;

	/* Fill out the rte_eth_stats statistics structure */
	stats->ipackets = total_qprc;
	stats->ibytes = total_qbrc;
	stats->opackets = hw_stats->gptc;
	stats->obytes = hw_stats->gotc;

	for (i = 0; i < IXGBE_QUEUE_STAT_COUNTERS; i++) {
		stats->q_ipackets[i] = hw_stats->qprc[i];
		stats->q_opackets[i] = hw_stats->qptc[i];
		stats->q_ibytes[i] = hw_stats->qbrc[i];
		stats->q_obytes[i] = hw_stats->qbtc[i];
		stats->q_errors[i] = hw_stats->qprdc[i];
	}

	/* Rx Errors */
	stats->imissed  = total_missed_rx;
	stats->ierrors  = hw_stats->crcerrs +
	                  hw_stats->mspdc +
	                  hw_stats->rlec +
	                  hw_stats->ruc +
	                  hw_stats->roc +
	                  total_missed_rx +
	                  hw_stats->illerrc +
	                  hw_stats->errbc +
	                  hw_stats->rfc +
	                  hw_stats->fccrc +
	                  hw_stats->fclast;

	/* Tx Errors */
	stats->oerrors  = 0;
}

static void
ixgbe_dev_stats_reset(struct rte_eth_dev *dev)
{
	struct ixgbe_hw_stats *stats =
			IXGBE_DEV_PRIVATE_TO_STATS(dev->data->dev_private);

	/* HW registers are cleared on read */
	ixgbe_dev_stats_get(dev, NULL);

	/* Reset software totals */
	memset(stats, 0, sizeof(*stats));
}

/* This function calculates the number of xstats based on the current config */
static unsigned
ixgbe_xstats_calc_num(void) {
	return IXGBE_NB_HW_STATS + (IXGBE_NB_RXQ_PRIO_STATS * 8) +
		(IXGBE_NB_TXQ_PRIO_STATS * 8);
}

static int
ixgbe_dev_xstats_get(struct rte_eth_dev *dev, struct rte_eth_xstats *xstats,
					 unsigned n)
{
	struct ixgbe_hw *hw =
			IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_hw_stats *hw_stats =
			IXGBE_DEV_PRIVATE_TO_STATS(dev->data->dev_private);
	uint64_t total_missed_rx, total_qbrc, total_qprc, total_qprdc;
	unsigned i, stat, count = 0;

	count = ixgbe_xstats_calc_num();

	if (n < count)
		return count;

	total_missed_rx = 0;
	total_qbrc = 0;
	total_qprc = 0;
	total_qprdc = 0;

	ixgbe_read_stats_registers(hw, hw_stats, &total_missed_rx, &total_qbrc,
				   &total_qprc, &total_qprdc);

	/* If this is a reset xstats is NULL, and we have cleared the
	 * registers by reading them.
	 */
	if (!xstats)
		return 0;

	/* Extended stats from ixgbe_hw_stats */
	count = 0;
	for (i = 0; i < IXGBE_NB_HW_STATS; i++) {
		snprintf(xstats[count].name, sizeof(xstats[count].name), "%s",
			 rte_ixgbe_stats_strings[i].name);
		xstats[count].value = *(uint64_t *)(((char *)hw_stats) +
				rte_ixgbe_stats_strings[i].offset);
		count++;
	}

	/* RX Priority Stats */
	for (stat = 0; stat < IXGBE_NB_RXQ_PRIO_STATS; stat++) {
		for (i = 0; i < 8; i++) {
			snprintf(xstats[count].name, sizeof(xstats[count].name),
				 "rx_priority%u_%s", i,
				 rte_ixgbe_rxq_strings[stat].name);
			xstats[count].value = *(uint64_t *)(((char *)hw_stats) +
					rte_ixgbe_rxq_strings[stat].offset +
					(sizeof(uint64_t) * i));
			count++;
		}
	}

	/* TX Priority Stats */
	for (stat = 0; stat < IXGBE_NB_TXQ_PRIO_STATS; stat++) {
		for (i = 0; i < 8; i++) {
			snprintf(xstats[count].name, sizeof(xstats[count].name),
				 "tx_priority%u_%s", i,
				 rte_ixgbe_txq_strings[stat].name);
			xstats[count].value = *(uint64_t *)(((char *)hw_stats) +
					rte_ixgbe_txq_strings[stat].offset +
					(sizeof(uint64_t) * i));
			count++;
		}
	}

	return count;
}

static void
ixgbe_dev_xstats_reset(struct rte_eth_dev *dev)
{
	struct ixgbe_hw_stats *stats =
			IXGBE_DEV_PRIVATE_TO_STATS(dev->data->dev_private);

	unsigned count = ixgbe_xstats_calc_num();

	/* HW registers are cleared on read */
	ixgbe_dev_xstats_get(dev, NULL, count);

	/* Reset software totals */
	memset(stats, 0, sizeof(*stats));
}

static void
ixgbevf_update_stats(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbevf_hw_stats *hw_stats = (struct ixgbevf_hw_stats*)
			  IXGBE_DEV_PRIVATE_TO_STATS(dev->data->dev_private);

	/* Good Rx packet, include VF loopback */
	UPDATE_VF_STAT(IXGBE_VFGPRC,
	    hw_stats->last_vfgprc, hw_stats->vfgprc);

	/* Good Rx octets, include VF loopback */
	UPDATE_VF_STAT_36BIT(IXGBE_VFGORC_LSB, IXGBE_VFGORC_MSB,
	    hw_stats->last_vfgorc, hw_stats->vfgorc);

	/* Good Tx packet, include VF loopback */
	UPDATE_VF_STAT(IXGBE_VFGPTC,
	    hw_stats->last_vfgptc, hw_stats->vfgptc);

	/* Good Tx octets, include VF loopback */
	UPDATE_VF_STAT_36BIT(IXGBE_VFGOTC_LSB, IXGBE_VFGOTC_MSB,
	    hw_stats->last_vfgotc, hw_stats->vfgotc);

	/* Rx Multicst Packet */
	UPDATE_VF_STAT(IXGBE_VFMPRC,
	    hw_stats->last_vfmprc, hw_stats->vfmprc);
}

static int
ixgbevf_dev_xstats_get(struct rte_eth_dev *dev, struct rte_eth_xstats *xstats,
		       unsigned n)
{
	struct ixgbevf_hw_stats *hw_stats = (struct ixgbevf_hw_stats *)
			IXGBE_DEV_PRIVATE_TO_STATS(dev->data->dev_private);
	unsigned i;

	if (n < IXGBEVF_NB_XSTATS)
		return IXGBEVF_NB_XSTATS;

	ixgbevf_update_stats(dev);

	if (!xstats)
		return 0;

	/* Extended stats */
	for (i = 0; i < IXGBEVF_NB_XSTATS; i++) {
		snprintf(xstats[i].name, sizeof(xstats[i].name),
			 "%s", rte_ixgbevf_stats_strings[i].name);
		xstats[i].value = *(uint64_t *)(((char *)hw_stats) +
			rte_ixgbevf_stats_strings[i].offset);
	}

	return IXGBEVF_NB_XSTATS;
}

static void
ixgbevf_dev_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	struct ixgbevf_hw_stats *hw_stats = (struct ixgbevf_hw_stats *)
			  IXGBE_DEV_PRIVATE_TO_STATS(dev->data->dev_private);

	ixgbevf_update_stats(dev);

	if (stats == NULL)
		return;

	stats->ipackets = hw_stats->vfgprc;
	stats->ibytes = hw_stats->vfgorc;
	stats->opackets = hw_stats->vfgptc;
	stats->obytes = hw_stats->vfgotc;
	stats->imcasts = hw_stats->vfmprc;
	/* stats->imcasts should be removed as imcasts is deprecated */
}

static void
ixgbevf_dev_stats_reset(struct rte_eth_dev *dev)
{
	struct ixgbevf_hw_stats *hw_stats = (struct ixgbevf_hw_stats*)
			IXGBE_DEV_PRIVATE_TO_STATS(dev->data->dev_private);

	/* Sync HW register to the last stats */
	ixgbevf_dev_stats_get(dev, NULL);

	/* reset HW current stats*/
	hw_stats->vfgprc = 0;
	hw_stats->vfgorc = 0;
	hw_stats->vfgptc = 0;
	hw_stats->vfgotc = 0;
	hw_stats->vfmprc = 0;

}

static void
ixgbe_dev_info_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	dev_info->max_rx_queues = (uint16_t)hw->mac.max_rx_queues;
	dev_info->max_tx_queues = (uint16_t)hw->mac.max_tx_queues;
	dev_info->min_rx_bufsize = 1024; /* cf BSIZEPACKET in SRRCTL register */
	dev_info->max_rx_pktlen = 15872; /* includes CRC, cf MAXFRS register */
	dev_info->max_mac_addrs = hw->mac.num_rar_entries;
	dev_info->max_hash_mac_addrs = IXGBE_VMDQ_NUM_UC_MAC;
	dev_info->max_vfs = dev->pci_dev->max_vfs;
	if (hw->mac.type == ixgbe_mac_82598EB)
		dev_info->max_vmdq_pools = ETH_16_POOLS;
	else
		dev_info->max_vmdq_pools = ETH_64_POOLS;
	dev_info->vmdq_queue_num = dev_info->max_rx_queues;
	dev_info->rx_offload_capa =
		DEV_RX_OFFLOAD_VLAN_STRIP |
		DEV_RX_OFFLOAD_IPV4_CKSUM |
		DEV_RX_OFFLOAD_UDP_CKSUM  |
		DEV_RX_OFFLOAD_TCP_CKSUM;

	/*
	 * RSC is only supported by 82599 and x540 PF devices in a non-SR-IOV
	 * mode.
	 */
	if ((hw->mac.type == ixgbe_mac_82599EB ||
	     hw->mac.type == ixgbe_mac_X540) &&
	    !RTE_ETH_DEV_SRIOV(dev).active)
		dev_info->rx_offload_capa |= DEV_RX_OFFLOAD_TCP_LRO;

	dev_info->tx_offload_capa =
		DEV_TX_OFFLOAD_VLAN_INSERT |
		DEV_TX_OFFLOAD_IPV4_CKSUM  |
		DEV_TX_OFFLOAD_UDP_CKSUM   |
		DEV_TX_OFFLOAD_TCP_CKSUM   |
		DEV_TX_OFFLOAD_SCTP_CKSUM  |
		DEV_TX_OFFLOAD_TCP_TSO;

	dev_info->default_rxconf = (struct rte_eth_rxconf) {
		.rx_thresh = {
			.pthresh = IXGBE_DEFAULT_RX_PTHRESH,
			.hthresh = IXGBE_DEFAULT_RX_HTHRESH,
			.wthresh = IXGBE_DEFAULT_RX_WTHRESH,
		},
		.rx_free_thresh = IXGBE_DEFAULT_RX_FREE_THRESH,
		.rx_drop_en = 0,
	};

	dev_info->default_txconf = (struct rte_eth_txconf) {
		.tx_thresh = {
			.pthresh = IXGBE_DEFAULT_TX_PTHRESH,
			.hthresh = IXGBE_DEFAULT_TX_HTHRESH,
			.wthresh = IXGBE_DEFAULT_TX_WTHRESH,
		},
		.tx_free_thresh = IXGBE_DEFAULT_TX_FREE_THRESH,
		.tx_rs_thresh = IXGBE_DEFAULT_TX_RSBIT_THRESH,
		.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS |
				ETH_TXQ_FLAGS_NOOFFLOADS,
	};

	dev_info->rx_desc_lim = rx_desc_lim;
	dev_info->tx_desc_lim = tx_desc_lim;

	dev_info->hash_key_size = IXGBE_HKEY_MAX_INDEX * sizeof(uint32_t);
	dev_info->reta_size = ixgbe_reta_size_get(hw->mac.type);
	dev_info->flow_type_rss_offloads = IXGBE_RSS_OFFLOAD_ALL;
}

static void
ixgbevf_dev_info_get(struct rte_eth_dev *dev,
		     struct rte_eth_dev_info *dev_info)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	dev_info->max_rx_queues = (uint16_t)hw->mac.max_rx_queues;
	dev_info->max_tx_queues = (uint16_t)hw->mac.max_tx_queues;
	dev_info->min_rx_bufsize = 1024; /* cf BSIZEPACKET in SRRCTL reg */
	dev_info->max_rx_pktlen = 15872; /* includes CRC, cf MAXFRS reg */
	dev_info->max_mac_addrs = hw->mac.num_rar_entries;
	dev_info->max_hash_mac_addrs = IXGBE_VMDQ_NUM_UC_MAC;
	dev_info->max_vfs = dev->pci_dev->max_vfs;
	if (hw->mac.type == ixgbe_mac_82598EB)
		dev_info->max_vmdq_pools = ETH_16_POOLS;
	else
		dev_info->max_vmdq_pools = ETH_64_POOLS;
	dev_info->rx_offload_capa = DEV_RX_OFFLOAD_VLAN_STRIP |
				DEV_RX_OFFLOAD_IPV4_CKSUM |
				DEV_RX_OFFLOAD_UDP_CKSUM  |
				DEV_RX_OFFLOAD_TCP_CKSUM;
	dev_info->tx_offload_capa = DEV_TX_OFFLOAD_VLAN_INSERT |
				DEV_TX_OFFLOAD_IPV4_CKSUM  |
				DEV_TX_OFFLOAD_UDP_CKSUM   |
				DEV_TX_OFFLOAD_TCP_CKSUM   |
				DEV_TX_OFFLOAD_SCTP_CKSUM  |
				DEV_TX_OFFLOAD_TCP_TSO;

	dev_info->default_rxconf = (struct rte_eth_rxconf) {
		.rx_thresh = {
			.pthresh = IXGBE_DEFAULT_RX_PTHRESH,
			.hthresh = IXGBE_DEFAULT_RX_HTHRESH,
			.wthresh = IXGBE_DEFAULT_RX_WTHRESH,
		},
		.rx_free_thresh = IXGBE_DEFAULT_RX_FREE_THRESH,
		.rx_drop_en = 0,
	};

	dev_info->default_txconf = (struct rte_eth_txconf) {
		.tx_thresh = {
			.pthresh = IXGBE_DEFAULT_TX_PTHRESH,
			.hthresh = IXGBE_DEFAULT_TX_HTHRESH,
			.wthresh = IXGBE_DEFAULT_TX_WTHRESH,
		},
		.tx_free_thresh = IXGBE_DEFAULT_TX_FREE_THRESH,
		.tx_rs_thresh = IXGBE_DEFAULT_TX_RSBIT_THRESH,
		.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS |
				ETH_TXQ_FLAGS_NOOFFLOADS,
	};

	dev_info->rx_desc_lim = rx_desc_lim;
	dev_info->tx_desc_lim = tx_desc_lim;
}

/* return 0 means link status changed, -1 means not changed */
static int
ixgbe_dev_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct rte_eth_link link, old;
	ixgbe_link_speed link_speed = IXGBE_LINK_SPEED_UNKNOWN;
	int link_up;
	int diag;

	link.link_status = 0;
	link.link_speed = 0;
	link.link_duplex = 0;
	memset(&old, 0, sizeof(old));
	rte_ixgbe_dev_atomic_read_link_status(dev, &old);

	hw->mac.get_link_status = true;

	/* check if it needs to wait to complete, if lsc interrupt is enabled */
	if (wait_to_complete == 0 || dev->data->dev_conf.intr_conf.lsc != 0)
		diag = ixgbe_check_link(hw, &link_speed, &link_up, 0);
	else
		diag = ixgbe_check_link(hw, &link_speed, &link_up, 1);

	if (diag != 0) {
		link.link_speed = ETH_LINK_SPEED_100;
		link.link_duplex = ETH_LINK_HALF_DUPLEX;
		rte_ixgbe_dev_atomic_write_link_status(dev, &link);
		if (link.link_status == old.link_status)
			return -1;
		return 0;
	}

	if (link_up == 0) {
		rte_ixgbe_dev_atomic_write_link_status(dev, &link);
		if (link.link_status == old.link_status)
			return -1;
		return 0;
	}
	link.link_status = 1;
	link.link_duplex = ETH_LINK_FULL_DUPLEX;

	switch (link_speed) {
	default:
	case IXGBE_LINK_SPEED_UNKNOWN:
		link.link_duplex = ETH_LINK_HALF_DUPLEX;
		link.link_speed = ETH_LINK_SPEED_100;
		break;

	case IXGBE_LINK_SPEED_100_FULL:
		link.link_speed = ETH_LINK_SPEED_100;
		break;

	case IXGBE_LINK_SPEED_1GB_FULL:
		link.link_speed = ETH_LINK_SPEED_1000;
		break;

	case IXGBE_LINK_SPEED_10GB_FULL:
		link.link_speed = ETH_LINK_SPEED_10000;
		break;
	}
	rte_ixgbe_dev_atomic_write_link_status(dev, &link);

	if (link.link_status == old.link_status)
		return -1;

	return 0;
}

static void
ixgbe_dev_promiscuous_enable(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t fctrl;

	fctrl = IXGBE_READ_REG(hw, IXGBE_FCTRL);
	fctrl |= (IXGBE_FCTRL_UPE | IXGBE_FCTRL_MPE);
	IXGBE_WRITE_REG(hw, IXGBE_FCTRL, fctrl);
}

static void
ixgbe_dev_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t fctrl;

	fctrl = IXGBE_READ_REG(hw, IXGBE_FCTRL);
	fctrl &= (~IXGBE_FCTRL_UPE);
	if (dev->data->all_multicast == 1)
		fctrl |= IXGBE_FCTRL_MPE;
	else
		fctrl &= (~IXGBE_FCTRL_MPE);
	IXGBE_WRITE_REG(hw, IXGBE_FCTRL, fctrl);
}

static void
ixgbe_dev_allmulticast_enable(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t fctrl;

	fctrl = IXGBE_READ_REG(hw, IXGBE_FCTRL);
	fctrl |= IXGBE_FCTRL_MPE;
	IXGBE_WRITE_REG(hw, IXGBE_FCTRL, fctrl);
}

static void
ixgbe_dev_allmulticast_disable(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t fctrl;

	if (dev->data->promiscuous == 1)
		return; /* must remain in all_multicast mode */

	fctrl = IXGBE_READ_REG(hw, IXGBE_FCTRL);
	fctrl &= (~IXGBE_FCTRL_MPE);
	IXGBE_WRITE_REG(hw, IXGBE_FCTRL, fctrl);
}

/**
 * It clears the interrupt causes and enables the interrupt.
 * It will be called once only during nic initialized.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int
ixgbe_dev_lsc_interrupt_setup(struct rte_eth_dev *dev)
{
	struct ixgbe_interrupt *intr =
		IXGBE_DEV_PRIVATE_TO_INTR(dev->data->dev_private);

	ixgbe_dev_link_status_print(dev);
	intr->mask |= IXGBE_EICR_LSC;

	return 0;
}

/**
 * It clears the interrupt causes and enables the interrupt.
 * It will be called once only during nic initialized.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int
ixgbe_dev_rxq_interrupt_setup(struct rte_eth_dev *dev)
{
	struct ixgbe_interrupt *intr =
		IXGBE_DEV_PRIVATE_TO_INTR(dev->data->dev_private);

	intr->mask |= IXGBE_EICR_RTX_QUEUE;

	return 0;
}

/*
 * It reads ICR and sets flag (IXGBE_EICR_LSC) for the link_update.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int
ixgbe_dev_interrupt_get_status(struct rte_eth_dev *dev)
{
	uint32_t eicr;
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_interrupt *intr =
		IXGBE_DEV_PRIVATE_TO_INTR(dev->data->dev_private);

	/* clear all cause mask */
	ixgbe_disable_intr(hw);

	/* read-on-clear nic registers here */
	eicr = IXGBE_READ_REG(hw, IXGBE_EICR);
	PMD_DRV_LOG(DEBUG, "eicr %x", eicr);

	intr->flags = 0;

	/* set flag for async link update */
	if (eicr & IXGBE_EICR_LSC)
		intr->flags |= IXGBE_FLAG_NEED_LINK_UPDATE;

	if (eicr & IXGBE_EICR_MAILBOX)
		intr->flags |= IXGBE_FLAG_MAILBOX;

	return 0;
}

/**
 * It gets and then prints the link status.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static void
ixgbe_dev_link_status_print(struct rte_eth_dev *dev)
{
	struct rte_eth_link link;

	memset(&link, 0, sizeof(link));
	rte_ixgbe_dev_atomic_read_link_status(dev, &link);
	if (link.link_status) {
		PMD_INIT_LOG(INFO, "Port %d: Link Up - speed %u Mbps - %s",
					(int)(dev->data->port_id),
					(unsigned)link.link_speed,
			link.link_duplex == ETH_LINK_FULL_DUPLEX ?
					"full-duplex" : "half-duplex");
	} else {
		PMD_INIT_LOG(INFO, " Port %d: Link Down",
				(int)(dev->data->port_id));
	}
	PMD_INIT_LOG(DEBUG, "PCI Address: %04d:%02d:%02d:%d",
				dev->pci_dev->addr.domain,
				dev->pci_dev->addr.bus,
				dev->pci_dev->addr.devid,
				dev->pci_dev->addr.function);
}

/*
 * It executes link_update after knowing an interrupt occurred.
 *
 * @param dev
 *  Pointer to struct rte_eth_dev.
 *
 * @return
 *  - On success, zero.
 *  - On failure, a negative value.
 */
static int
ixgbe_dev_interrupt_action(struct rte_eth_dev *dev)
{
	struct ixgbe_interrupt *intr =
		IXGBE_DEV_PRIVATE_TO_INTR(dev->data->dev_private);
	int64_t timeout;
	struct rte_eth_link link;
	int intr_enable_delay = false;

	PMD_DRV_LOG(DEBUG, "intr action type %d", intr->flags);

	if (intr->flags & IXGBE_FLAG_MAILBOX) {
		ixgbe_pf_mbx_process(dev);
		intr->flags &= ~IXGBE_FLAG_MAILBOX;
	}

	if (intr->flags & IXGBE_FLAG_NEED_LINK_UPDATE) {
		/* get the link status before link update, for predicting later */
		memset(&link, 0, sizeof(link));
		rte_ixgbe_dev_atomic_read_link_status(dev, &link);

		ixgbe_dev_link_update(dev, 0);

		/* likely to up */
		if (!link.link_status)
			/* handle it 1 sec later, wait it being stable */
			timeout = IXGBE_LINK_UP_CHECK_TIMEOUT;
		/* likely to down */
		else
			/* handle it 4 sec later, wait it being stable */
			timeout = IXGBE_LINK_DOWN_CHECK_TIMEOUT;

		ixgbe_dev_link_status_print(dev);

		intr_enable_delay = true;
	}

	if (intr_enable_delay) {
		if (rte_eal_alarm_set(timeout * 1000,
				      ixgbe_dev_interrupt_delayed_handler, (void*)dev) < 0)
			PMD_DRV_LOG(ERR, "Error setting alarm");
	} else {
		PMD_DRV_LOG(DEBUG, "enable intr immediately");
		ixgbe_enable_intr(dev);
		rte_intr_enable(&(dev->pci_dev->intr_handle));
	}


	return 0;
}

/**
 * Interrupt handler which shall be registered for alarm callback for delayed
 * handling specific interrupt to wait for the stable nic state. As the
 * NIC interrupt state is not stable for ixgbe after link is just down,
 * it needs to wait 4 seconds to get the stable status.
 *
 * @param handle
 *  Pointer to interrupt handle.
 * @param param
 *  The address of parameter (struct rte_eth_dev *) regsitered before.
 *
 * @return
 *  void
 */
static void
ixgbe_dev_interrupt_delayed_handler(void *param)
{
	struct rte_eth_dev *dev = (struct rte_eth_dev *)param;
	struct ixgbe_interrupt *intr =
		IXGBE_DEV_PRIVATE_TO_INTR(dev->data->dev_private);
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t eicr;

	eicr = IXGBE_READ_REG(hw, IXGBE_EICR);
	if (eicr & IXGBE_EICR_MAILBOX)
		ixgbe_pf_mbx_process(dev);

	if (intr->flags & IXGBE_FLAG_NEED_LINK_UPDATE) {
		ixgbe_dev_link_update(dev, 0);
		intr->flags &= ~IXGBE_FLAG_NEED_LINK_UPDATE;
		ixgbe_dev_link_status_print(dev);
		_rte_eth_dev_callback_process(dev, RTE_ETH_EVENT_INTR_LSC);
	}

	PMD_DRV_LOG(DEBUG, "enable intr in delayed handler S[%08x]", eicr);
	ixgbe_enable_intr(dev);
	rte_intr_enable(&(dev->pci_dev->intr_handle));
}

/**
 * Interrupt handler triggered by NIC  for handling
 * specific interrupt.
 *
 * @param handle
 *  Pointer to interrupt handle.
 * @param param
 *  The address of parameter (struct rte_eth_dev *) regsitered before.
 *
 * @return
 *  void
 */
static void
ixgbe_dev_interrupt_handler(__rte_unused struct rte_intr_handle *handle,
			    void *param)
{
	struct rte_eth_dev *dev = (struct rte_eth_dev *)param;

	ixgbe_dev_interrupt_get_status(dev);
	ixgbe_dev_interrupt_action(dev);
}

static int
ixgbe_dev_led_on(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw;

	hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	return ixgbe_led_on(hw, 0) == IXGBE_SUCCESS ? 0 : -ENOTSUP;
}

static int
ixgbe_dev_led_off(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw;

	hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	return ixgbe_led_off(hw, 0) == IXGBE_SUCCESS ? 0 : -ENOTSUP;
}

static int
ixgbe_flow_ctrl_get(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	struct ixgbe_hw *hw;
	uint32_t mflcn_reg;
	uint32_t fccfg_reg;
	int rx_pause;
	int tx_pause;

	hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	fc_conf->pause_time = hw->fc.pause_time;
	fc_conf->high_water = hw->fc.high_water[0];
	fc_conf->low_water = hw->fc.low_water[0];
	fc_conf->send_xon = hw->fc.send_xon;
	fc_conf->autoneg = !hw->fc.disable_fc_autoneg;

	/*
	 * Return rx_pause status according to actual setting of
	 * MFLCN register.
	 */
	mflcn_reg = IXGBE_READ_REG(hw, IXGBE_MFLCN);
	if (mflcn_reg & (IXGBE_MFLCN_RPFCE | IXGBE_MFLCN_RFCE))
		rx_pause = 1;
	else
		rx_pause = 0;

	/*
	 * Return tx_pause status according to actual setting of
	 * FCCFG register.
	 */
	fccfg_reg = IXGBE_READ_REG(hw, IXGBE_FCCFG);
	if (fccfg_reg & (IXGBE_FCCFG_TFCE_802_3X | IXGBE_FCCFG_TFCE_PRIORITY))
		tx_pause = 1;
	else
		tx_pause = 0;

	if (rx_pause && tx_pause)
		fc_conf->mode = RTE_FC_FULL;
	else if (rx_pause)
		fc_conf->mode = RTE_FC_RX_PAUSE;
	else if (tx_pause)
		fc_conf->mode = RTE_FC_TX_PAUSE;
	else
		fc_conf->mode = RTE_FC_NONE;

	return 0;
}

static int
ixgbe_flow_ctrl_set(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	struct ixgbe_hw *hw;
	int err;
	uint32_t rx_buf_size;
	uint32_t max_high_water;
	uint32_t mflcn;
	enum ixgbe_fc_mode rte_fcmode_2_ixgbe_fcmode[] = {
		ixgbe_fc_none,
		ixgbe_fc_rx_pause,
		ixgbe_fc_tx_pause,
		ixgbe_fc_full
	};

	PMD_INIT_FUNC_TRACE();

	hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	rx_buf_size = IXGBE_READ_REG(hw, IXGBE_RXPBSIZE(0));
	PMD_INIT_LOG(DEBUG, "Rx packet buffer size = 0x%x", rx_buf_size);

	/*
	 * At least reserve one Ethernet frame for watermark
	 * high_water/low_water in kilo bytes for ixgbe
	 */
	max_high_water = (rx_buf_size - ETHER_MAX_LEN) >> IXGBE_RXPBSIZE_SHIFT;
	if ((fc_conf->high_water > max_high_water) ||
		(fc_conf->high_water < fc_conf->low_water)) {
		PMD_INIT_LOG(ERR, "Invalid high/low water setup value in KB");
		PMD_INIT_LOG(ERR, "High_water must <= 0x%x", max_high_water);
		return -EINVAL;
	}

	hw->fc.requested_mode = rte_fcmode_2_ixgbe_fcmode[fc_conf->mode];
	hw->fc.pause_time     = fc_conf->pause_time;
	hw->fc.high_water[0]  = fc_conf->high_water;
	hw->fc.low_water[0]   = fc_conf->low_water;
	hw->fc.send_xon       = fc_conf->send_xon;
	hw->fc.disable_fc_autoneg = !fc_conf->autoneg;

	err = ixgbe_fc_enable(hw);

	/* Not negotiated is not an error case */
	if ((err == IXGBE_SUCCESS) || (err == IXGBE_ERR_FC_NOT_NEGOTIATED)) {

		/* check if we want to forward MAC frames - driver doesn't have native
		 * capability to do that, so we'll write the registers ourselves */

		mflcn = IXGBE_READ_REG(hw, IXGBE_MFLCN);

		/* set or clear MFLCN.PMCF bit depending on configuration */
		if (fc_conf->mac_ctrl_frame_fwd != 0)
			mflcn |= IXGBE_MFLCN_PMCF;
		else
			mflcn &= ~IXGBE_MFLCN_PMCF;

		IXGBE_WRITE_REG(hw, IXGBE_MFLCN, mflcn);
		IXGBE_WRITE_FLUSH(hw);

		return 0;
	}

	PMD_INIT_LOG(ERR, "ixgbe_fc_enable = 0x%x", err);
	return -EIO;
}

/**
 *  ixgbe_pfc_enable_generic - Enable flow control
 *  @hw: pointer to hardware structure
 *  @tc_num: traffic class number
 *  Enable flow control according to the current settings.
 */
static int
ixgbe_dcb_pfc_enable_generic(struct ixgbe_hw *hw,uint8_t tc_num)
{
	int ret_val = 0;
	uint32_t mflcn_reg, fccfg_reg;
	uint32_t reg;
	uint32_t fcrtl, fcrth;
	uint8_t i;
	uint8_t nb_rx_en;

	/* Validate the water mark configuration */
	if (!hw->fc.pause_time) {
		ret_val = IXGBE_ERR_INVALID_LINK_SETTINGS;
		goto out;
	}

	/* Low water mark of zero causes XOFF floods */
	if (hw->fc.current_mode & ixgbe_fc_tx_pause) {
		 /* High/Low water can not be 0 */
		if( (!hw->fc.high_water[tc_num])|| (!hw->fc.low_water[tc_num])) {
			PMD_INIT_LOG(ERR, "Invalid water mark configuration");
			ret_val = IXGBE_ERR_INVALID_LINK_SETTINGS;
			goto out;
		}

		if(hw->fc.low_water[tc_num] >= hw->fc.high_water[tc_num]) {
			PMD_INIT_LOG(ERR, "Invalid water mark configuration");
			ret_val = IXGBE_ERR_INVALID_LINK_SETTINGS;
			goto out;
		}
	}
	/* Negotiate the fc mode to use */
	ixgbe_fc_autoneg(hw);

	/* Disable any previous flow control settings */
	mflcn_reg = IXGBE_READ_REG(hw, IXGBE_MFLCN);
	mflcn_reg &= ~(IXGBE_MFLCN_RPFCE_SHIFT | IXGBE_MFLCN_RFCE|IXGBE_MFLCN_RPFCE);

	fccfg_reg = IXGBE_READ_REG(hw, IXGBE_FCCFG);
	fccfg_reg &= ~(IXGBE_FCCFG_TFCE_802_3X | IXGBE_FCCFG_TFCE_PRIORITY);

	switch (hw->fc.current_mode) {
	case ixgbe_fc_none:
		/*
		 * If the count of enabled RX Priority Flow control >1,
		 * and the TX pause can not be disabled
		 */
		nb_rx_en = 0;
		for (i =0; i < IXGBE_DCB_MAX_TRAFFIC_CLASS; i++) {
			reg = IXGBE_READ_REG(hw, IXGBE_FCRTH_82599(i));
			if (reg & IXGBE_FCRTH_FCEN)
				nb_rx_en++;
		}
		if (nb_rx_en > 1)
			fccfg_reg |=IXGBE_FCCFG_TFCE_PRIORITY;
		break;
	case ixgbe_fc_rx_pause:
		/*
		 * Rx Flow control is enabled and Tx Flow control is
		 * disabled by software override. Since there really
		 * isn't a way to advertise that we are capable of RX
		 * Pause ONLY, we will advertise that we support both
		 * symmetric and asymmetric Rx PAUSE.  Later, we will
		 * disable the adapter's ability to send PAUSE frames.
		 */
		mflcn_reg |= IXGBE_MFLCN_RPFCE;
		/*
		 * If the count of enabled RX Priority Flow control >1,
		 * and the TX pause can not be disabled
		 */
		nb_rx_en = 0;
		for (i =0; i < IXGBE_DCB_MAX_TRAFFIC_CLASS; i++) {
			reg = IXGBE_READ_REG(hw, IXGBE_FCRTH_82599(i));
			if (reg & IXGBE_FCRTH_FCEN)
				nb_rx_en++;
		}
		if (nb_rx_en > 1)
			fccfg_reg |=IXGBE_FCCFG_TFCE_PRIORITY;
		break;
	case ixgbe_fc_tx_pause:
		/*
		 * Tx Flow control is enabled, and Rx Flow control is
		 * disabled by software override.
		 */
		fccfg_reg |=IXGBE_FCCFG_TFCE_PRIORITY;
		break;
	case ixgbe_fc_full:
		/* Flow control (both Rx and Tx) is enabled by SW override. */
		mflcn_reg |= IXGBE_MFLCN_RPFCE;
		fccfg_reg |= IXGBE_FCCFG_TFCE_PRIORITY;
		break;
	default:
		PMD_DRV_LOG(DEBUG, "Flow control param set incorrectly");
		ret_val = IXGBE_ERR_CONFIG;
		goto out;
		break;
	}

	/* Set 802.3x based flow control settings. */
	mflcn_reg |= IXGBE_MFLCN_DPF;
	IXGBE_WRITE_REG(hw, IXGBE_MFLCN, mflcn_reg);
	IXGBE_WRITE_REG(hw, IXGBE_FCCFG, fccfg_reg);

	/* Set up and enable Rx high/low water mark thresholds, enable XON. */
	if ((hw->fc.current_mode & ixgbe_fc_tx_pause) &&
		hw->fc.high_water[tc_num]) {
		fcrtl = (hw->fc.low_water[tc_num] << 10) | IXGBE_FCRTL_XONE;
		IXGBE_WRITE_REG(hw, IXGBE_FCRTL_82599(tc_num), fcrtl);
		fcrth = (hw->fc.high_water[tc_num] << 10) | IXGBE_FCRTH_FCEN;
	} else {
		IXGBE_WRITE_REG(hw, IXGBE_FCRTL_82599(tc_num), 0);
		/*
		 * In order to prevent Tx hangs when the internal Tx
		 * switch is enabled we must set the high water mark
		 * to the maximum FCRTH value.  This allows the Tx
		 * switch to function even under heavy Rx workloads.
		 */
		fcrth = IXGBE_READ_REG(hw, IXGBE_RXPBSIZE(tc_num)) - 32;
	}
	IXGBE_WRITE_REG(hw, IXGBE_FCRTH_82599(tc_num), fcrth);

	/* Configure pause time (2 TCs per register) */
	reg = hw->fc.pause_time * 0x00010001;
	for (i = 0; i < (IXGBE_DCB_MAX_TRAFFIC_CLASS / 2); i++)
		IXGBE_WRITE_REG(hw, IXGBE_FCTTV(i), reg);

	/* Configure flow control refresh threshold value */
	IXGBE_WRITE_REG(hw, IXGBE_FCRTV, hw->fc.pause_time / 2);

out:
	return ret_val;
}

static int
ixgbe_dcb_pfc_enable(struct rte_eth_dev *dev,uint8_t tc_num)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	int32_t ret_val = IXGBE_NOT_IMPLEMENTED;

	if(hw->mac.type != ixgbe_mac_82598EB) {
		ret_val = ixgbe_dcb_pfc_enable_generic(hw,tc_num);
	}
	return ret_val;
}

static int
ixgbe_priority_flow_ctrl_set(struct rte_eth_dev *dev, struct rte_eth_pfc_conf *pfc_conf)
{
	int err;
	uint32_t rx_buf_size;
	uint32_t max_high_water;
	uint8_t tc_num;
	uint8_t  map[IXGBE_DCB_MAX_USER_PRIORITY] = { 0 };
	struct ixgbe_hw *hw =
                IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_dcb_config *dcb_config =
                IXGBE_DEV_PRIVATE_TO_DCB_CFG(dev->data->dev_private);

	enum ixgbe_fc_mode rte_fcmode_2_ixgbe_fcmode[] = {
		ixgbe_fc_none,
		ixgbe_fc_rx_pause,
		ixgbe_fc_tx_pause,
		ixgbe_fc_full
	};

	PMD_INIT_FUNC_TRACE();

	ixgbe_dcb_unpack_map_cee(dcb_config, IXGBE_DCB_RX_CONFIG, map);
	tc_num = map[pfc_conf->priority];
	rx_buf_size = IXGBE_READ_REG(hw, IXGBE_RXPBSIZE(tc_num));
	PMD_INIT_LOG(DEBUG, "Rx packet buffer size = 0x%x", rx_buf_size);
	/*
	 * At least reserve one Ethernet frame for watermark
	 * high_water/low_water in kilo bytes for ixgbe
	 */
	max_high_water = (rx_buf_size - ETHER_MAX_LEN) >> IXGBE_RXPBSIZE_SHIFT;
	if ((pfc_conf->fc.high_water > max_high_water) ||
	    (pfc_conf->fc.high_water <= pfc_conf->fc.low_water)) {
		PMD_INIT_LOG(ERR, "Invalid high/low water setup value in KB");
		PMD_INIT_LOG(ERR, "High_water must <= 0x%x", max_high_water);
		return -EINVAL;
	}

	hw->fc.requested_mode = rte_fcmode_2_ixgbe_fcmode[pfc_conf->fc.mode];
	hw->fc.pause_time = pfc_conf->fc.pause_time;
	hw->fc.send_xon = pfc_conf->fc.send_xon;
	hw->fc.low_water[tc_num] =  pfc_conf->fc.low_water;
	hw->fc.high_water[tc_num] = pfc_conf->fc.high_water;

	err = ixgbe_dcb_pfc_enable(dev,tc_num);

	/* Not negotiated is not an error case */
	if ((err == IXGBE_SUCCESS) || (err == IXGBE_ERR_FC_NOT_NEGOTIATED))
		return 0;

	PMD_INIT_LOG(ERR, "ixgbe_dcb_pfc_enable = 0x%x", err);
	return -EIO;
}

static int
ixgbe_dev_rss_reta_update(struct rte_eth_dev *dev,
			  struct rte_eth_rss_reta_entry64 *reta_conf,
			  uint16_t reta_size)
{
	uint8_t i, j, mask;
	uint32_t reta, r;
	uint16_t idx, shift;
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint16_t sp_reta_size;
	uint32_t reta_reg;

	PMD_INIT_FUNC_TRACE();

	if (!ixgbe_rss_update_sp(hw->mac.type)) {
		PMD_DRV_LOG(ERR, "RSS reta update is not supported on this "
			"NIC.");
		return -ENOTSUP;
	}

	sp_reta_size = ixgbe_reta_size_get(hw->mac.type);
	if (reta_size != sp_reta_size) {
		PMD_DRV_LOG(ERR, "The size of hash lookup table configured "
			"(%d) doesn't match the number hardware can supported "
			"(%d)\n", reta_size, sp_reta_size);
		return -EINVAL;
	}

	for (i = 0; i < reta_size; i += IXGBE_4_BIT_WIDTH) {
		idx = i / RTE_RETA_GROUP_SIZE;
		shift = i % RTE_RETA_GROUP_SIZE;
		mask = (uint8_t)((reta_conf[idx].mask >> shift) &
						IXGBE_4_BIT_MASK);
		if (!mask)
			continue;
		reta_reg = ixgbe_reta_reg_get(hw->mac.type, i);
		if (mask == IXGBE_4_BIT_MASK)
			r = 0;
		else
			r = IXGBE_READ_REG(hw, reta_reg);
		for (j = 0, reta = 0; j < IXGBE_4_BIT_WIDTH; j++) {
			if (mask & (0x1 << j))
				reta |= reta_conf[idx].reta[shift + j] <<
							(CHAR_BIT * j);
			else
				reta |= r & (IXGBE_8_BIT_MASK <<
						(CHAR_BIT * j));
		}
		IXGBE_WRITE_REG(hw, reta_reg, reta);
	}

	return 0;
}

static int
ixgbe_dev_rss_reta_query(struct rte_eth_dev *dev,
			 struct rte_eth_rss_reta_entry64 *reta_conf,
			 uint16_t reta_size)
{
	uint8_t i, j, mask;
	uint32_t reta;
	uint16_t idx, shift;
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint16_t sp_reta_size;
	uint32_t reta_reg;

	PMD_INIT_FUNC_TRACE();
	sp_reta_size = ixgbe_reta_size_get(hw->mac.type);
	if (reta_size != sp_reta_size) {
		PMD_DRV_LOG(ERR, "The size of hash lookup table configured "
			"(%d) doesn't match the number hardware can supported "
			"(%d)\n", reta_size, sp_reta_size);
		return -EINVAL;
	}

	for (i = 0; i < reta_size; i += IXGBE_4_BIT_WIDTH) {
		idx = i / RTE_RETA_GROUP_SIZE;
		shift = i % RTE_RETA_GROUP_SIZE;
		mask = (uint8_t)((reta_conf[idx].mask >> shift) &
						IXGBE_4_BIT_MASK);
		if (!mask)
			continue;

		reta_reg = ixgbe_reta_reg_get(hw->mac.type, i);
		reta = IXGBE_READ_REG(hw, reta_reg);
		for (j = 0; j < IXGBE_4_BIT_WIDTH; j++) {
			if (mask & (0x1 << j))
				reta_conf[idx].reta[shift + j] =
					((reta >> (CHAR_BIT * j)) &
						IXGBE_8_BIT_MASK);
		}
	}

	return 0;
}

static void
ixgbe_add_rar(struct rte_eth_dev *dev, struct ether_addr *mac_addr,
				uint32_t index, uint32_t pool)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t enable_addr = 1;

	ixgbe_set_rar(hw, index, mac_addr->addr_bytes, pool, enable_addr);
}

static void
ixgbe_remove_rar(struct rte_eth_dev *dev, uint32_t index)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	ixgbe_clear_rar(hw, index);
}

static void
ixgbe_set_default_mac_addr(struct rte_eth_dev *dev, struct ether_addr *addr)
{
	ixgbe_remove_rar(dev, 0);

	ixgbe_add_rar(dev, addr, 0, 0);
}

static int
ixgbe_dev_mtu_set(struct rte_eth_dev *dev, uint16_t mtu)
{
	uint32_t hlreg0;
	uint32_t maxfrs;
	struct ixgbe_hw *hw;
	struct rte_eth_dev_info dev_info;
	uint32_t frame_size = mtu + ETHER_HDR_LEN + ETHER_CRC_LEN;

	ixgbe_dev_info_get(dev, &dev_info);

	/* check that mtu is within the allowed range */
	if ((mtu < ETHER_MIN_MTU) || (frame_size > dev_info.max_rx_pktlen))
		return -EINVAL;

	/* refuse mtu that requires the support of scattered packets when this
	 * feature has not been enabled before. */
	if (!dev->data->scattered_rx &&
	    (frame_size + 2 * IXGBE_VLAN_TAG_SIZE >
	     dev->data->min_rx_buf_size - RTE_PKTMBUF_HEADROOM))
		return -EINVAL;

	hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	hlreg0 = IXGBE_READ_REG(hw, IXGBE_HLREG0);

	/* switch to jumbo mode if needed */
	if (frame_size > ETHER_MAX_LEN) {
		dev->data->dev_conf.rxmode.jumbo_frame = 1;
		hlreg0 |= IXGBE_HLREG0_JUMBOEN;
	} else {
		dev->data->dev_conf.rxmode.jumbo_frame = 0;
		hlreg0 &= ~IXGBE_HLREG0_JUMBOEN;
	}
	IXGBE_WRITE_REG(hw, IXGBE_HLREG0, hlreg0);

	/* update max frame size */
	dev->data->dev_conf.rxmode.max_rx_pkt_len = frame_size;

	maxfrs = IXGBE_READ_REG(hw, IXGBE_MAXFRS);
	maxfrs &= 0x0000FFFF;
	maxfrs |= (dev->data->dev_conf.rxmode.max_rx_pkt_len << 16);
	IXGBE_WRITE_REG(hw, IXGBE_MAXFRS, maxfrs);

	return 0;
}

/*
 * Virtual Function operations
 */
static void
ixgbevf_intr_disable(struct ixgbe_hw *hw)
{
	PMD_INIT_FUNC_TRACE();

	/* Clear interrupt mask to stop from interrupts being generated */
	IXGBE_WRITE_REG(hw, IXGBE_VTEIMC, IXGBE_VF_IRQ_CLEAR_MASK);

	IXGBE_WRITE_FLUSH(hw);
}

static void
ixgbevf_intr_enable(struct ixgbe_hw *hw)
{
	PMD_INIT_FUNC_TRACE();

	/* VF enable interrupt autoclean */
	IXGBE_WRITE_REG(hw, IXGBE_VTEIAM, IXGBE_VF_IRQ_ENABLE_MASK);
	IXGBE_WRITE_REG(hw, IXGBE_VTEIAC, IXGBE_VF_IRQ_ENABLE_MASK);
	IXGBE_WRITE_REG(hw, IXGBE_VTEIMS, IXGBE_VF_IRQ_ENABLE_MASK);

	IXGBE_WRITE_FLUSH(hw);
}

static int
ixgbevf_dev_configure(struct rte_eth_dev *dev)
{
	struct rte_eth_conf* conf = &dev->data->dev_conf;
	struct ixgbe_adapter *adapter =
			(struct ixgbe_adapter *)dev->data->dev_private;

	PMD_INIT_LOG(DEBUG, "Configured Virtual Function port id: %d",
		     dev->data->port_id);

	/*
	 * VF has no ability to enable/disable HW CRC
	 * Keep the persistent behavior the same as Host PF
	 */
#ifndef RTE_LIBRTE_IXGBE_PF_DISABLE_STRIP_CRC
	if (!conf->rxmode.hw_strip_crc) {
		PMD_INIT_LOG(NOTICE, "VF can't disable HW CRC Strip");
		conf->rxmode.hw_strip_crc = 1;
	}
#else
	if (conf->rxmode.hw_strip_crc) {
		PMD_INIT_LOG(NOTICE, "VF can't enable HW CRC Strip");
		conf->rxmode.hw_strip_crc = 0;
	}
#endif

	/*
	 * Initialize to TRUE. If any of Rx queues doesn't meet the bulk
	 * allocation or vector Rx preconditions we will reset it.
	 */
	adapter->rx_bulk_alloc_allowed = true;
	adapter->rx_vec_allowed = true;

	return 0;
}

static int
ixgbevf_dev_start(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t intr_vector = 0;
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;

	int err, mask = 0;

	PMD_INIT_FUNC_TRACE();

	hw->mac.ops.reset_hw(hw);
	hw->mac.get_link_status = true;

	/* negotiate mailbox API version to use with the PF. */
	ixgbevf_negotiate_api(hw);

	ixgbevf_dev_tx_init(dev);

	/* This can fail when allocating mbufs for descriptor rings */
	err = ixgbevf_dev_rx_init(dev);
	if (err) {
		PMD_INIT_LOG(ERR, "Unable to initialize RX hardware (%d)", err);
		ixgbe_dev_clear_queues(dev);
		return err;
	}

	/* Set vfta */
	ixgbevf_set_vfta_all(dev,1);

	/* Set HW strip */
	mask = ETH_VLAN_STRIP_MASK | ETH_VLAN_FILTER_MASK | \
		ETH_VLAN_EXTEND_MASK;
	ixgbevf_vlan_offload_set(dev, mask);

	ixgbevf_dev_rxtx_start(dev);

	/* check and configure queue intr-vector mapping */
	if (dev->data->dev_conf.intr_conf.rxq != 0) {
		intr_vector = dev->data->nb_rx_queues;
		if (rte_intr_efd_enable(intr_handle, intr_vector))
			return -1;
	}

	if (rte_intr_dp_is_en(intr_handle) && !intr_handle->intr_vec) {
		intr_handle->intr_vec =
			rte_zmalloc("intr_vec",
				    dev->data->nb_rx_queues * sizeof(int), 0);
		if (intr_handle->intr_vec == NULL) {
			PMD_INIT_LOG(ERR, "Failed to allocate %d rx_queues"
				     " intr_vec\n", dev->data->nb_rx_queues);
			return -ENOMEM;
		}
	}
	ixgbevf_configure_msix(dev);

	rte_intr_enable(intr_handle);

	/* Re-enable interrupt for VF */
	ixgbevf_intr_enable(hw);

	return 0;
}

static void
ixgbevf_dev_stop(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;

	PMD_INIT_FUNC_TRACE();

	hw->adapter_stopped = 1;
	ixgbe_stop_adapter(hw);

	/*
	  * Clear what we set, but we still keep shadow_vfta to
	  * restore after device starts
	  */
	ixgbevf_set_vfta_all(dev,0);

	/* Clear stored conf */
	dev->data->scattered_rx = 0;

	ixgbe_dev_clear_queues(dev);

	/* disable intr eventfd mapping */
	rte_intr_disable(intr_handle);

	/* Clean datapath event and queue/vec mapping */
	rte_intr_efd_disable(intr_handle);
	if (intr_handle->intr_vec != NULL) {
		rte_free(intr_handle->intr_vec);
		intr_handle->intr_vec = NULL;
	}
}

static void
ixgbevf_dev_close(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	PMD_INIT_FUNC_TRACE();

	ixgbe_reset_hw(hw);

	ixgbevf_dev_stop(dev);

	ixgbe_dev_free_queues(dev);

	/* reprogram the RAR[0] in case user changed it. */
	ixgbe_set_rar(hw, 0, hw->mac.addr, 0, IXGBE_RAH_AV);
}

static void ixgbevf_set_vfta_all(struct rte_eth_dev *dev, bool on)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_vfta * shadow_vfta =
		IXGBE_DEV_PRIVATE_TO_VFTA(dev->data->dev_private);
	int i = 0, j = 0, vfta = 0, mask = 1;

	for (i = 0; i < IXGBE_VFTA_SIZE; i++){
		vfta = shadow_vfta->vfta[i];
		if(vfta){
			mask = 1;
			for (j = 0; j < 32; j++){
				if(vfta & mask)
					ixgbe_set_vfta(hw, (i<<5)+j, 0, on);
				mask<<=1;
			}
		}
	}

}

static int
ixgbevf_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_vfta * shadow_vfta =
		IXGBE_DEV_PRIVATE_TO_VFTA(dev->data->dev_private);
	uint32_t vid_idx = 0;
	uint32_t vid_bit = 0;
	int ret = 0;

	PMD_INIT_FUNC_TRACE();

	/* vind is not used in VF driver, set to 0, check ixgbe_set_vfta_vf */
	ret = ixgbe_set_vfta(hw, vlan_id, 0, !!on);
	if(ret){
		PMD_INIT_LOG(ERR, "Unable to set VF vlan");
		return ret;
	}
	vid_idx = (uint32_t) ((vlan_id >> 5) & 0x7F);
	vid_bit = (uint32_t) (1 << (vlan_id & 0x1F));

	/* Save what we set and retore it after device reset */
	if (on)
		shadow_vfta->vfta[vid_idx] |= vid_bit;
	else
		shadow_vfta->vfta[vid_idx] &= ~vid_bit;

	return 0;
}

static void
ixgbevf_vlan_strip_queue_set(struct rte_eth_dev *dev, uint16_t queue, int on)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t ctrl;

	PMD_INIT_FUNC_TRACE();

	if(queue >= hw->mac.max_rx_queues)
		return;

	ctrl = IXGBE_READ_REG(hw, IXGBE_RXDCTL(queue));
	if(on)
		ctrl |= IXGBE_RXDCTL_VME;
	else
		ctrl &= ~IXGBE_RXDCTL_VME;
	IXGBE_WRITE_REG(hw, IXGBE_RXDCTL(queue), ctrl);

	ixgbe_vlan_hw_strip_bitmap_set( dev, queue, on);
}

static void
ixgbevf_vlan_offload_set(struct rte_eth_dev *dev, int mask)
{
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint16_t i;
	int on = 0;

	/* VF function only support hw strip feature, others are not support */
	if(mask & ETH_VLAN_STRIP_MASK){
		on = !!(dev->data->dev_conf.rxmode.hw_vlan_strip);

		for(i=0; i < hw->mac.max_rx_queues; i++)
			ixgbevf_vlan_strip_queue_set(dev,i,on);
	}
}

static int
ixgbe_vmdq_mode_check(struct ixgbe_hw *hw)
{
	uint32_t reg_val;

	/* we only need to do this if VMDq is enabled */
	reg_val = IXGBE_READ_REG(hw, IXGBE_VT_CTL);
	if (!(reg_val & IXGBE_VT_CTL_VT_ENABLE)) {
		PMD_INIT_LOG(ERR, "VMDq must be enabled for this setting");
		return -1;
	}

	return 0;
}

static uint32_t
ixgbe_uta_vector(struct ixgbe_hw *hw, struct ether_addr* uc_addr)
{
	uint32_t vector = 0;
	switch (hw->mac.mc_filter_type) {
	case 0:   /* use bits [47:36] of the address */
		vector = ((uc_addr->addr_bytes[4] >> 4) |
			(((uint16_t)uc_addr->addr_bytes[5]) << 4));
		break;
	case 1:   /* use bits [46:35] of the address */
		vector = ((uc_addr->addr_bytes[4] >> 3) |
			(((uint16_t)uc_addr->addr_bytes[5]) << 5));
		break;
	case 2:   /* use bits [45:34] of the address */
		vector = ((uc_addr->addr_bytes[4] >> 2) |
			(((uint16_t)uc_addr->addr_bytes[5]) << 6));
		break;
	case 3:   /* use bits [43:32] of the address */
		vector = ((uc_addr->addr_bytes[4]) |
			(((uint16_t)uc_addr->addr_bytes[5]) << 8));
		break;
	default:  /* Invalid mc_filter_type */
		break;
	}

	/* vector can only be 12-bits or boundary will be exceeded */
	vector &= 0xFFF;
	return vector;
}

static int
ixgbe_uc_hash_table_set(struct rte_eth_dev *dev,struct ether_addr* mac_addr,
			       uint8_t on)
{
	uint32_t vector;
	uint32_t uta_idx;
	uint32_t reg_val;
	uint32_t uta_shift;
	uint32_t rc;
	const uint32_t ixgbe_uta_idx_mask = 0x7F;
	const uint32_t ixgbe_uta_bit_shift = 5;
	const uint32_t ixgbe_uta_bit_mask = (0x1 << ixgbe_uta_bit_shift) - 1;
	const uint32_t bit1 = 0x1;

	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_uta_info *uta_info =
		IXGBE_DEV_PRIVATE_TO_UTA(dev->data->dev_private);

	/* The UTA table only exists on 82599 hardware and newer */
	if (hw->mac.type < ixgbe_mac_82599EB)
		return -ENOTSUP;

	vector = ixgbe_uta_vector(hw,mac_addr);
	uta_idx = (vector >> ixgbe_uta_bit_shift) & ixgbe_uta_idx_mask;
	uta_shift = vector & ixgbe_uta_bit_mask;

	rc = ((uta_info->uta_shadow[uta_idx] >> uta_shift & bit1) != 0);
	if(rc == on)
		return 0;

	reg_val = IXGBE_READ_REG(hw, IXGBE_UTA(uta_idx));
	if (on) {
		uta_info->uta_in_use++;
		reg_val |= (bit1 << uta_shift);
		uta_info->uta_shadow[uta_idx] |= (bit1 << uta_shift);
	} else {
		uta_info->uta_in_use--;
		reg_val &= ~(bit1 << uta_shift);
		uta_info->uta_shadow[uta_idx] &= ~(bit1 << uta_shift);
	}

	IXGBE_WRITE_REG(hw, IXGBE_UTA(uta_idx), reg_val);

	if (uta_info->uta_in_use > 0)
		IXGBE_WRITE_REG(hw, IXGBE_MCSTCTRL,
				IXGBE_MCSTCTRL_MFE | hw->mac.mc_filter_type);
	else
		IXGBE_WRITE_REG(hw, IXGBE_MCSTCTRL,hw->mac.mc_filter_type);

	return 0;
}

static int
ixgbe_uc_all_hash_table_set(struct rte_eth_dev *dev, uint8_t on)
{
	int i;
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_uta_info *uta_info =
		IXGBE_DEV_PRIVATE_TO_UTA(dev->data->dev_private);

	/* The UTA table only exists on 82599 hardware and newer */
	if (hw->mac.type < ixgbe_mac_82599EB)
		return -ENOTSUP;

	if(on) {
		for (i = 0; i < ETH_VMDQ_NUM_UC_HASH_ARRAY; i++) {
			uta_info->uta_shadow[i] = ~0;
			IXGBE_WRITE_REG(hw, IXGBE_UTA(i), ~0);
		}
	} else {
		for (i = 0; i < ETH_VMDQ_NUM_UC_HASH_ARRAY; i++) {
			uta_info->uta_shadow[i] = 0;
			IXGBE_WRITE_REG(hw, IXGBE_UTA(i), 0);
		}
	}
	return 0;

}

uint32_t
ixgbe_convert_vm_rx_mask_to_val(uint16_t rx_mask, uint32_t orig_val)
{
	uint32_t new_val = orig_val;

	if (rx_mask & ETH_VMDQ_ACCEPT_UNTAG)
		new_val |= IXGBE_VMOLR_AUPE;
	if (rx_mask & ETH_VMDQ_ACCEPT_HASH_MC)
		new_val |= IXGBE_VMOLR_ROMPE;
	if (rx_mask & ETH_VMDQ_ACCEPT_HASH_UC)
		new_val |= IXGBE_VMOLR_ROPE;
	if (rx_mask & ETH_VMDQ_ACCEPT_BROADCAST)
		new_val |= IXGBE_VMOLR_BAM;
	if (rx_mask & ETH_VMDQ_ACCEPT_MULTICAST)
		new_val |= IXGBE_VMOLR_MPE;

	return new_val;
}

static int
ixgbe_set_pool_rx_mode(struct rte_eth_dev *dev, uint16_t pool,
			       uint16_t rx_mask, uint8_t on)
{
	int val = 0;

	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t vmolr = IXGBE_READ_REG(hw, IXGBE_VMOLR(pool));

	if (hw->mac.type == ixgbe_mac_82598EB) {
		PMD_INIT_LOG(ERR, "setting VF receive mode set should be done"
			     " on 82599 hardware and newer");
		return -ENOTSUP;
	}
	if (ixgbe_vmdq_mode_check(hw) < 0)
		return -ENOTSUP;

	val = ixgbe_convert_vm_rx_mask_to_val(rx_mask, val);

	if (on)
		vmolr |= val;
	else
		vmolr &= ~val;

	IXGBE_WRITE_REG(hw, IXGBE_VMOLR(pool), vmolr);

	return 0;
}

static int
ixgbe_set_pool_rx(struct rte_eth_dev *dev, uint16_t pool, uint8_t on)
{
	uint32_t reg,addr;
	uint32_t val;
	const uint8_t bit1 = 0x1;

	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	if (ixgbe_vmdq_mode_check(hw) < 0)
		return -ENOTSUP;

	addr = IXGBE_VFRE(pool >= ETH_64_POOLS/2);
	reg = IXGBE_READ_REG(hw, addr);
	val = bit1 << pool;

	if (on)
		reg |= val;
	else
		reg &= ~val;

	IXGBE_WRITE_REG(hw, addr,reg);

	return 0;
}

static int
ixgbe_set_pool_tx(struct rte_eth_dev *dev, uint16_t pool, uint8_t on)
{
	uint32_t reg,addr;
	uint32_t val;
	const uint8_t bit1 = 0x1;

	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	if (ixgbe_vmdq_mode_check(hw) < 0)
		return -ENOTSUP;

	addr = IXGBE_VFTE(pool >= ETH_64_POOLS/2);
	reg = IXGBE_READ_REG(hw, addr);
	val = bit1 << pool;

	if (on)
		reg |= val;
	else
		reg &= ~val;

	IXGBE_WRITE_REG(hw, addr,reg);

	return 0;
}

static int
ixgbe_set_pool_vlan_filter(struct rte_eth_dev *dev, uint16_t vlan,
			uint64_t pool_mask, uint8_t vlan_on)
{
	int ret = 0;
	uint16_t pool_idx;
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	if (ixgbe_vmdq_mode_check(hw) < 0)
		return -ENOTSUP;
	for (pool_idx = 0; pool_idx < ETH_64_POOLS; pool_idx++) {
		if (pool_mask & ((uint64_t)(1ULL << pool_idx)))
			ret = hw->mac.ops.set_vfta(hw,vlan,pool_idx,vlan_on);
			if (ret < 0)
				return ret;
	}

	return ret;
}
 
static int
ixgbe_set_pool_vlan_anti_spoof(struct rte_eth_dev *dev,
			uint32_t vf, uint8_t on)
{
	int ret = 0;
	struct ixgbe_hw *hw =
	IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
    
	int vf_target_reg = vf >> 3;
	int vf_target_shift = vf % 8 + IXGBE_SPOOF_VLANAS_SHIFT;
	u32 pfvfspoof;

	if (hw->mac.type == ixgbe_mac_82598EB)
		return 0;
  
	pfvfspoof = IXGBE_READ_REG(hw, IXGBE_PFVFSPOOF(vf_target_reg));
   
	if (on)
		pfvfspoof |= (1 << vf_target_shift);
	else
		pfvfspoof &= ~(1 << vf_target_shift);
	
	IXGBE_WRITE_REG(hw, IXGBE_PFVFSPOOF(vf_target_reg), pfvfspoof);
	pfvfspoof = IXGBE_READ_REG(hw, IXGBE_PFVFSPOOF(vf_target_reg));

	return ret;
}

static int
ixgbe_set_pool_mac_anti_spoof(struct rte_eth_dev *dev,
			uint32_t vf, uint8_t on)
{
	int ret = 0;
	
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
    
	int vf_target_reg = vf >> 3;
	int vf_target_shift = vf % 8;
	u32 pfvfspoof;

	if (hw->mac.type == ixgbe_mac_82598EB)
		return 0;
  
	pfvfspoof = IXGBE_READ_REG(hw, IXGBE_PFVFSPOOF(vf_target_reg));
  
	if (on)
		pfvfspoof |= (1 << vf_target_shift);
	else
		pfvfspoof &= ~(1 << vf_target_shift);
	IXGBE_WRITE_REG(hw, IXGBE_PFVFSPOOF(vf_target_reg), pfvfspoof);
	
	pfvfspoof = IXGBE_READ_REG(hw, IXGBE_PFVFSPOOF(vf_target_reg));
	
	return ret;
}

static int 
ixgbe_ping_vfs(struct rte_eth_dev *dev, int32_t vf)
{
	int ret = 0;   
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	struct ixgbe_vf_info *vfinfo =
		*IXGBE_DEV_PRIVATE_TO_P_VFDATA(dev->data->dev_private);
    
	u32 ping;
	int i;
  
	for (i = 0; i < dev->pci_dev->max_vfs; i++) {
		ping = IXGBE_PF_CONTROL_MSG;
		if (vfinfo[i].clear_to_send)
			ping |= IXGBE_VT_MSGTYPE_CTS;
    
		/* ping every VF or only one specified */
		if (vf < 0 || vf == i)
			ixgbe_write_mbx(hw, &ping, 1, i);
	}

	return ret;
}

#define IXGBE_MRCTL_VPME  0x01 /* Virtual Pool Mirroring. */
#define IXGBE_MRCTL_UPME  0x02 /* Uplink Port Mirroring. */
#define IXGBE_MRCTL_DPME  0x04 /* Downlink Port Mirroring. */
#define IXGBE_MRCTL_VLME  0x08 /* VLAN Mirroring. */
#define IXGBE_INVALID_MIRROR_TYPE(mirror_type) \
	((mirror_type) & ~(uint8_t)(ETH_MIRROR_VIRTUAL_POOL_UP | \
	ETH_MIRROR_UPLINK_PORT | ETH_MIRROR_DOWNLINK_PORT | ETH_MIRROR_VLAN))

static int
ixgbe_mirror_rule_set(struct rte_eth_dev *dev,
			struct rte_eth_mirror_conf *mirror_conf,
			uint8_t rule_id, uint8_t on)
{
	uint32_t mr_ctl,vlvf;
	uint32_t mp_lsb = 0;
	uint32_t mv_msb = 0;
	uint32_t mv_lsb = 0;
	uint32_t mp_msb = 0;
	uint8_t i = 0;
	int reg_index = 0;
	uint64_t vlan_mask = 0;

	const uint8_t pool_mask_offset = 32;
	const uint8_t vlan_mask_offset = 32;
	const uint8_t dst_pool_offset = 8;
	const uint8_t rule_mr_offset  = 4;
	const uint8_t mirror_rule_mask= 0x0F;

	struct ixgbe_mirror_info *mr_info =
			(IXGBE_DEV_PRIVATE_TO_PFDATA(dev->data->dev_private));
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint8_t mirror_type = 0;

	if (ixgbe_vmdq_mode_check(hw) < 0)
		return -ENOTSUP;

	if (rule_id >= IXGBE_MAX_MIRROR_RULES)
		return -EINVAL;

	if (IXGBE_INVALID_MIRROR_TYPE(mirror_conf->rule_type)) {
		PMD_DRV_LOG(ERR, "unsupported mirror type 0x%x.",
			mirror_conf->rule_type);
		return -EINVAL;
	}

	if (mirror_conf->rule_type & ETH_MIRROR_VLAN) {
		mirror_type |= IXGBE_MRCTL_VLME;
		/* Check if vlan id is valid and find conresponding VLAN ID index in VLVF */
		for (i = 0;i < IXGBE_VLVF_ENTRIES; i++) {
			if (mirror_conf->vlan.vlan_mask & (1ULL << i)) {
				/* search vlan id related pool vlan filter index */
				reg_index = ixgbe_find_vlvf_slot(hw,
						mirror_conf->vlan.vlan_id[i]);
				if(reg_index < 0)
					return -EINVAL;
				vlvf = IXGBE_READ_REG(hw, IXGBE_VLVF(reg_index));
				if ((vlvf & IXGBE_VLVF_VIEN) &&
				    ((vlvf & IXGBE_VLVF_VLANID_MASK) ==
				      mirror_conf->vlan.vlan_id[i]))
					vlan_mask |= (1ULL << reg_index);
				else
					return -EINVAL;
			}
		}

		if (on) {
			mv_lsb = vlan_mask & 0xFFFFFFFF;
			mv_msb = vlan_mask >> vlan_mask_offset;

			mr_info->mr_conf[rule_id].vlan.vlan_mask =
						mirror_conf->vlan.vlan_mask;
			for(i = 0 ;i < ETH_VMDQ_MAX_VLAN_FILTERS; i++) {
				if(mirror_conf->vlan.vlan_mask & (1ULL << i))
					mr_info->mr_conf[rule_id].vlan.vlan_id[i] =
						mirror_conf->vlan.vlan_id[i];
			}
		} else {
			mv_lsb = 0;
			mv_msb = 0;
			mr_info->mr_conf[rule_id].vlan.vlan_mask = 0;
			for(i = 0 ;i < ETH_VMDQ_MAX_VLAN_FILTERS; i++)
				mr_info->mr_conf[rule_id].vlan.vlan_id[i] = 0;
		}
	}

	/*
	 * if enable pool mirror, write related pool mask register,if disable
	 * pool mirror, clear PFMRVM register
	 */
	if (mirror_conf->rule_type & ETH_MIRROR_VIRTUAL_POOL_UP) {
		mirror_type |= IXGBE_MRCTL_VPME;
		if (on) {
			mp_lsb = mirror_conf->pool_mask & 0xFFFFFFFF;
			mp_msb = mirror_conf->pool_mask >> pool_mask_offset;
			mr_info->mr_conf[rule_id].pool_mask =
					mirror_conf->pool_mask;

		} else {
			mp_lsb = 0;
			mp_msb = 0;
			mr_info->mr_conf[rule_id].pool_mask = 0;
		}
	}
	if (mirror_conf->rule_type & ETH_MIRROR_UPLINK_PORT)
		mirror_type |= IXGBE_MRCTL_UPME;
	if (mirror_conf->rule_type & ETH_MIRROR_DOWNLINK_PORT)
		mirror_type |= IXGBE_MRCTL_DPME;

	/* read  mirror control register and recalculate it */
	mr_ctl = IXGBE_READ_REG(hw, IXGBE_MRCTL(rule_id));

	if (on) {
		mr_ctl |= mirror_type;
		mr_ctl &= mirror_rule_mask;
		mr_ctl |= mirror_conf->dst_pool << dst_pool_offset;
	} else
		mr_ctl &= ~(mirror_conf->rule_type & mirror_rule_mask);

	mr_info->mr_conf[rule_id].rule_type = mirror_conf->rule_type;
	mr_info->mr_conf[rule_id].dst_pool = mirror_conf->dst_pool;

	/* write mirrror control  register */
	IXGBE_WRITE_REG(hw, IXGBE_MRCTL(rule_id), mr_ctl);

	/* write pool mirrror control  register */
	if (mirror_conf->rule_type == ETH_MIRROR_VIRTUAL_POOL_UP) {
		IXGBE_WRITE_REG(hw, IXGBE_VMRVM(rule_id), mp_lsb);
		IXGBE_WRITE_REG(hw, IXGBE_VMRVM(rule_id + rule_mr_offset),
				mp_msb);
	}
	/* write VLAN mirrror control  register */
	if (mirror_conf->rule_type == ETH_MIRROR_VLAN) {
		IXGBE_WRITE_REG(hw, IXGBE_VMRVLAN(rule_id), mv_lsb);
		IXGBE_WRITE_REG(hw, IXGBE_VMRVLAN(rule_id + rule_mr_offset),
				mv_msb);
	}

	return 0;
}

static int
ixgbe_mirror_rule_reset(struct rte_eth_dev *dev, uint8_t rule_id)
{
	int mr_ctl = 0;
	uint32_t lsb_val = 0;
	uint32_t msb_val = 0;
	const uint8_t rule_mr_offset = 4;

	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_mirror_info *mr_info =
		(IXGBE_DEV_PRIVATE_TO_PFDATA(dev->data->dev_private));

	if (ixgbe_vmdq_mode_check(hw) < 0)
		return -ENOTSUP;

	memset(&mr_info->mr_conf[rule_id], 0,
		sizeof(struct rte_eth_mirror_conf));

	/* clear PFVMCTL register */
	IXGBE_WRITE_REG(hw, IXGBE_MRCTL(rule_id), mr_ctl);

	/* clear pool mask register */
	IXGBE_WRITE_REG(hw, IXGBE_VMRVM(rule_id), lsb_val);
	IXGBE_WRITE_REG(hw, IXGBE_VMRVM(rule_id + rule_mr_offset), msb_val);

	/* clear vlan mask register */
	IXGBE_WRITE_REG(hw, IXGBE_VMRVLAN(rule_id), lsb_val);
	IXGBE_WRITE_REG(hw, IXGBE_VMRVLAN(rule_id + rule_mr_offset), msb_val);

	return 0;
}

static int
ixgbevf_dev_rx_queue_intr_enable(struct rte_eth_dev *dev, uint16_t queue_id)
{
	uint32_t mask;
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	mask = IXGBE_READ_REG(hw, IXGBE_VTEIMS);
	mask |= (1 << IXGBE_MISC_VEC_ID);
	RTE_SET_USED(queue_id);
	IXGBE_WRITE_REG(hw, IXGBE_VTEIMS, mask);

	rte_intr_enable(&dev->pci_dev->intr_handle);

	return 0;
}

static int
ixgbevf_dev_rx_queue_intr_disable(struct rte_eth_dev *dev, uint16_t queue_id)
{
	uint32_t mask;
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	mask = IXGBE_READ_REG(hw, IXGBE_VTEIMS);
	mask &= ~(1 << IXGBE_MISC_VEC_ID);
	RTE_SET_USED(queue_id);
	IXGBE_WRITE_REG(hw, IXGBE_VTEIMS, mask);

	return 0;
}

static int
ixgbe_dev_rx_queue_intr_enable(struct rte_eth_dev *dev, uint16_t queue_id)
{
	uint32_t mask;
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_interrupt *intr =
		IXGBE_DEV_PRIVATE_TO_INTR(dev->data->dev_private);

	if (queue_id < 16) {
		ixgbe_disable_intr(hw);
		intr->mask |= (1 << queue_id);
		ixgbe_enable_intr(dev);
	} else if (queue_id < 32) {
		mask = IXGBE_READ_REG(hw, IXGBE_EIMS_EX(0));
		mask &= (1 << queue_id);
		IXGBE_WRITE_REG(hw, IXGBE_EIMS_EX(0), mask);
	} else if (queue_id < 64) {
		mask = IXGBE_READ_REG(hw, IXGBE_EIMS_EX(1));
		mask &= (1 << (queue_id - 32));
		IXGBE_WRITE_REG(hw, IXGBE_EIMS_EX(1), mask);
	}
	rte_intr_enable(&dev->pci_dev->intr_handle);

	return 0;
}

static int
ixgbe_dev_rx_queue_intr_disable(struct rte_eth_dev *dev, uint16_t queue_id)
{
	uint32_t mask;
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_interrupt *intr =
		IXGBE_DEV_PRIVATE_TO_INTR(dev->data->dev_private);

	if (queue_id < 16) {
		ixgbe_disable_intr(hw);
		intr->mask &= ~(1 << queue_id);
		ixgbe_enable_intr(dev);
	} else if (queue_id < 32) {
		mask = IXGBE_READ_REG(hw, IXGBE_EIMS_EX(0));
		mask &= ~(1 << queue_id);
		IXGBE_WRITE_REG(hw, IXGBE_EIMS_EX(0), mask);
	} else if (queue_id < 64) {
		mask = IXGBE_READ_REG(hw, IXGBE_EIMS_EX(1));
		mask &= ~(1 << (queue_id - 32));
		IXGBE_WRITE_REG(hw, IXGBE_EIMS_EX(1), mask);
	}

	return 0;
}

static void
ixgbevf_set_ivar_map(struct ixgbe_hw *hw, int8_t direction,
		     uint8_t queue, uint8_t msix_vector)
{
	uint32_t tmp, idx;

	if (direction == -1) {
		/* other causes */
		msix_vector |= IXGBE_IVAR_ALLOC_VAL;
		tmp = IXGBE_READ_REG(hw, IXGBE_VTIVAR_MISC);
		tmp &= ~0xFF;
		tmp |= msix_vector;
		IXGBE_WRITE_REG(hw, IXGBE_VTIVAR_MISC, tmp);
	} else {
		/* rx or tx cause */
		msix_vector |= IXGBE_IVAR_ALLOC_VAL;
		idx = ((16 * (queue & 1)) + (8 * direction));
		tmp = IXGBE_READ_REG(hw, IXGBE_VTIVAR(queue >> 1));
		tmp &= ~(0xFF << idx);
		tmp |= (msix_vector << idx);
		IXGBE_WRITE_REG(hw, IXGBE_VTIVAR(queue >> 1), tmp);
	}
}

/**
 * set the IVAR registers, mapping interrupt causes to vectors
 * @param hw
 *  pointer to ixgbe_hw struct
 * @direction
 *  0 for Rx, 1 for Tx, -1 for other causes
 * @queue
 *  queue to map the corresponding interrupt to
 * @msix_vector
 *  the vector to map to the corresponding queue
 */
static void
ixgbe_set_ivar_map(struct ixgbe_hw *hw, int8_t direction,
		   uint8_t queue, uint8_t msix_vector)
{
	uint32_t tmp, idx;

	msix_vector |= IXGBE_IVAR_ALLOC_VAL;
	if (hw->mac.type == ixgbe_mac_82598EB) {
		if (direction == -1)
			direction = 0;
		idx = (((direction * 64) + queue) >> 2) & 0x1F;
		tmp = IXGBE_READ_REG(hw, IXGBE_IVAR(idx));
		tmp &= ~(0xFF << (8 * (queue & 0x3)));
		tmp |= (msix_vector << (8 * (queue & 0x3)));
		IXGBE_WRITE_REG(hw, IXGBE_IVAR(idx), tmp);
	} else if ((hw->mac.type == ixgbe_mac_82599EB) ||
			(hw->mac.type == ixgbe_mac_X540)) {
		if (direction == -1) {
			/* other causes */
			idx = ((queue & 1) * 8);
			tmp = IXGBE_READ_REG(hw, IXGBE_IVAR_MISC);
			tmp &= ~(0xFF << idx);
			tmp |= (msix_vector << idx);
			IXGBE_WRITE_REG(hw, IXGBE_IVAR_MISC, tmp);
		} else {
			/* rx or tx causes */
			idx = ((16 * (queue & 1)) + (8 * direction));
			tmp = IXGBE_READ_REG(hw, IXGBE_IVAR(queue >> 1));
			tmp &= ~(0xFF << idx);
			tmp |= (msix_vector << idx);
			IXGBE_WRITE_REG(hw, IXGBE_IVAR(queue >> 1), tmp);
		}
	}
}

static void
ixgbevf_configure_msix(struct rte_eth_dev *dev)
{
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t q_idx;
	uint32_t vector_idx = IXGBE_MISC_VEC_ID;

	/* won't configure msix register if no mapping is done
	 * between intr vector and event fd.
	 */
	if (!rte_intr_dp_is_en(intr_handle))
		return;

	/* Configure all RX queues of VF */
	for (q_idx = 0; q_idx < dev->data->nb_rx_queues; q_idx++) {
		/* Force all queue use vector 0,
		 * as IXGBE_VF_MAXMSIVECOTR = 1
		 */
		ixgbevf_set_ivar_map(hw, 0, q_idx, vector_idx);
		intr_handle->intr_vec[q_idx] = vector_idx;
	}

	/* Configure VF other cause ivar */
	ixgbevf_set_ivar_map(hw, -1, 1, vector_idx);
}

/**
 * Sets up the hardware to properly generate MSI-X interrupts
 * @hw
 *  board private structure
 */
static void
ixgbe_configure_msix(struct rte_eth_dev *dev)
{
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;
	struct ixgbe_hw *hw =
		IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t queue_id, base = IXGBE_MISC_VEC_ID;
	uint32_t vec = IXGBE_MISC_VEC_ID;
	uint32_t mask;
	uint32_t gpie;

	/* won't configure msix register if no mapping is done
	 * between intr vector and event fd
	 */
	if (!rte_intr_dp_is_en(intr_handle))
		return;

	if (rte_intr_allow_others(intr_handle))
		vec = base = IXGBE_RX_VEC_START;

	/* setup GPIE for MSI-x mode */
	gpie = IXGBE_READ_REG(hw, IXGBE_GPIE);
	gpie |= IXGBE_GPIE_MSIX_MODE | IXGBE_GPIE_PBA_SUPPORT |
		IXGBE_GPIE_OCD | IXGBE_GPIE_EIAME;
	/* auto clearing and auto setting corresponding bits in EIMS
	 * when MSI-X interrupt is triggered
	 */
	if (hw->mac.type == ixgbe_mac_82598EB) {
		IXGBE_WRITE_REG(hw, IXGBE_EIAM, IXGBE_EICS_RTX_QUEUE);
	} else {
		IXGBE_WRITE_REG(hw, IXGBE_EIAM_EX(0), 0xFFFFFFFF);
		IXGBE_WRITE_REG(hw, IXGBE_EIAM_EX(1), 0xFFFFFFFF);
	}
	IXGBE_WRITE_REG(hw, IXGBE_GPIE, gpie);

	/* Populate the IVAR table and set the ITR values to the
	 * corresponding register.
	 */
	for (queue_id = 0; queue_id < dev->data->nb_rx_queues;
	     queue_id++) {
		/* by default, 1:1 mapping */
		ixgbe_set_ivar_map(hw, 0, queue_id, vec);
		intr_handle->intr_vec[queue_id] = vec;
		if (vec < base + intr_handle->nb_efd - 1)
			vec++;
	}

	switch (hw->mac.type) {
	case ixgbe_mac_82598EB:
		ixgbe_set_ivar_map(hw, -1, IXGBE_IVAR_OTHER_CAUSES_INDEX,
				   IXGBE_MISC_VEC_ID);
		break;
	case ixgbe_mac_82599EB:
	case ixgbe_mac_X540:
		ixgbe_set_ivar_map(hw, -1, 1, IXGBE_MISC_VEC_ID);
		break;
	default:
		break;
	}
	IXGBE_WRITE_REG(hw, IXGBE_EITR(IXGBE_MISC_VEC_ID),
			IXGBE_MIN_INTER_INTERRUPT_INTERVAL_DEFAULT & 0xFFF);

	/* set up to autoclear timer, and the vectors */
	mask = IXGBE_EIMS_ENABLE_MASK;
	mask &= ~(IXGBE_EIMS_OTHER |
		  IXGBE_EIMS_MAILBOX |
		  IXGBE_EIMS_LSC);

	IXGBE_WRITE_REG(hw, IXGBE_EIAC, mask);
}

static int ixgbe_set_queue_rate_limit(struct rte_eth_dev *dev,
	uint16_t queue_idx, uint16_t tx_rate)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t rf_dec, rf_int;
	uint32_t bcnrc_val;
	uint16_t link_speed = dev->data->dev_link.link_speed;

	if (queue_idx >= hw->mac.max_tx_queues)
		return -EINVAL;

	if (tx_rate != 0) {
		/* Calculate the rate factor values to set */
		rf_int = (uint32_t)link_speed / (uint32_t)tx_rate;
		rf_dec = (uint32_t)link_speed % (uint32_t)tx_rate;
		rf_dec = (rf_dec << IXGBE_RTTBCNRC_RF_INT_SHIFT) / tx_rate;

		bcnrc_val = IXGBE_RTTBCNRC_RS_ENA;
		bcnrc_val |= ((rf_int << IXGBE_RTTBCNRC_RF_INT_SHIFT) &
				IXGBE_RTTBCNRC_RF_INT_MASK_M);
		bcnrc_val |= (rf_dec & IXGBE_RTTBCNRC_RF_DEC_MASK);
	} else {
		bcnrc_val = 0;
	}

	/*
	 * Set global transmit compensation time to the MMW_SIZE in RTTBCNRM
	 * register. MMW_SIZE=0x014 if 9728-byte jumbo is supported, otherwise
	 * set as 0x4.
	 */
	if ((dev->data->dev_conf.rxmode.jumbo_frame == 1) &&
		(dev->data->dev_conf.rxmode.max_rx_pkt_len >=
				IXGBE_MAX_JUMBO_FRAME_SIZE))
		IXGBE_WRITE_REG(hw, IXGBE_RTTBCNRM,
			IXGBE_MMW_SIZE_JUMBO_FRAME);
	else
		IXGBE_WRITE_REG(hw, IXGBE_RTTBCNRM,
			IXGBE_MMW_SIZE_DEFAULT);

	/* Set RTTBCNRC of queue X */
	IXGBE_WRITE_REG(hw, IXGBE_RTTDQSEL, queue_idx);
	IXGBE_WRITE_REG(hw, IXGBE_RTTBCNRC, bcnrc_val);
	IXGBE_WRITE_FLUSH(hw);

	return 0;
}

static int ixgbe_set_vf_rate_limit(struct rte_eth_dev *dev, uint16_t vf,
	uint16_t tx_rate, uint64_t q_msk)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_vf_info *vfinfo =
		*(IXGBE_DEV_PRIVATE_TO_P_VFDATA(dev->data->dev_private));
	uint8_t  nb_q_per_pool = RTE_ETH_DEV_SRIOV(dev).nb_q_per_pool;
	uint32_t queue_stride =
		IXGBE_MAX_RX_QUEUE_NUM / RTE_ETH_DEV_SRIOV(dev).active;
	uint32_t queue_idx = vf * queue_stride, idx = 0, vf_idx;
	uint32_t queue_end = queue_idx + nb_q_per_pool - 1;
	uint16_t total_rate = 0;

	if (queue_end >= hw->mac.max_tx_queues)
		return -EINVAL;

	if (vfinfo != NULL) {
		for (vf_idx = 0; vf_idx < dev->pci_dev->max_vfs; vf_idx++) {
			if (vf_idx == vf)
				continue;
			for (idx = 0; idx < RTE_DIM(vfinfo[vf_idx].tx_rate);
				idx++)
				total_rate += vfinfo[vf_idx].tx_rate[idx];
		}
	} else
		return -EINVAL;

	/* Store tx_rate for this vf. */
	for (idx = 0; idx < nb_q_per_pool; idx++) {
		if (((uint64_t)0x1 << idx) & q_msk) {
			if (vfinfo[vf].tx_rate[idx] != tx_rate)
				vfinfo[vf].tx_rate[idx] = tx_rate;
			total_rate += tx_rate;
		}
	}

	if (total_rate > dev->data->dev_link.link_speed) {
		/*
		 * Reset stored TX rate of the VF if it causes exceed
		 * link speed.
		 */
		memset(vfinfo[vf].tx_rate, 0, sizeof(vfinfo[vf].tx_rate));
		return -EINVAL;
	}

	/* Set RTTBCNRC of each queue/pool for vf X  */
	for (; queue_idx <= queue_end; queue_idx++) {
		if (0x1 & q_msk)
			ixgbe_set_queue_rate_limit(dev, queue_idx, tx_rate);
		q_msk = q_msk >> 1;
	}

	return 0;
}

static void
ixgbevf_add_mac_addr(struct rte_eth_dev *dev, struct ether_addr *mac_addr,
		     __attribute__((unused)) uint32_t index,
		     __attribute__((unused)) uint32_t pool)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	int diag;

	/*
	 * On a 82599 VF, adding again the same MAC addr is not an idempotent
	 * operation. Trap this case to avoid exhausting the [very limited]
	 * set of PF resources used to store VF MAC addresses.
	 */
	if (memcmp(hw->mac.perm_addr, mac_addr, sizeof(struct ether_addr)) == 0)
		return;
	diag = ixgbevf_set_uc_addr_vf(hw, 2, mac_addr->addr_bytes);
	if (diag == 0)
		return;
	PMD_DRV_LOG(ERR, "Unable to add MAC address - diag=%d", diag);
}

static void
ixgbevf_remove_mac_addr(struct rte_eth_dev *dev, uint32_t index)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ether_addr *perm_addr = (struct ether_addr *) hw->mac.perm_addr;
	struct ether_addr *mac_addr;
	uint32_t i;
	int diag;

	/*
	 * The IXGBE_VF_SET_MACVLAN command of the ixgbe-pf driver does
	 * not support the deletion of a given MAC address.
	 * Instead, it imposes to delete all MAC addresses, then to add again
	 * all MAC addresses with the exception of the one to be deleted.
	 */
	(void) ixgbevf_set_uc_addr_vf(hw, 0, NULL);

	/*
	 * Add again all MAC addresses, with the exception of the deleted one
	 * and of the permanent MAC address.
	 */
	for (i = 0, mac_addr = dev->data->mac_addrs;
	     i < hw->mac.num_rar_entries; i++, mac_addr++) {
		/* Skip the deleted MAC address */
		if (i == index)
			continue;
		/* Skip NULL MAC addresses */
		if (is_zero_ether_addr(mac_addr))
			continue;
		/* Skip the permanent MAC address */
		if (memcmp(perm_addr, mac_addr, sizeof(struct ether_addr)) == 0)
			continue;
		diag = ixgbevf_set_uc_addr_vf(hw, 2, mac_addr->addr_bytes);
		if (diag != 0)
			PMD_DRV_LOG(ERR,
				    "Adding again MAC address "
				    "%02x:%02x:%02x:%02x:%02x:%02x failed "
				    "diag=%d",
				    mac_addr->addr_bytes[0],
				    mac_addr->addr_bytes[1],
				    mac_addr->addr_bytes[2],
				    mac_addr->addr_bytes[3],
				    mac_addr->addr_bytes[4],
				    mac_addr->addr_bytes[5],
				    diag);
	}
}

static void
ixgbevf_set_default_mac_addr(struct rte_eth_dev *dev, struct ether_addr *addr)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	hw->mac.ops.set_rar(hw, 0, (void *)addr, 0, 0);
}

#define MAC_TYPE_FILTER_SUP(type)    do {\
	if ((type) != ixgbe_mac_82599EB && (type) != ixgbe_mac_X540 &&\
		(type) != ixgbe_mac_X550)\
		return -ENOTSUP;\
} while (0)

static int
ixgbe_syn_filter_set(struct rte_eth_dev *dev,
			struct rte_eth_syn_filter *filter,
			bool add)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t synqf;

	if (filter->queue >= IXGBE_MAX_RX_QUEUE_NUM)
		return -EINVAL;

	synqf = IXGBE_READ_REG(hw, IXGBE_SYNQF);

	if (add) {
		if (synqf & IXGBE_SYN_FILTER_ENABLE)
			return -EINVAL;
		synqf = (uint32_t)(((filter->queue << IXGBE_SYN_FILTER_QUEUE_SHIFT) &
			IXGBE_SYN_FILTER_QUEUE) | IXGBE_SYN_FILTER_ENABLE);

		if (filter->hig_pri)
			synqf |= IXGBE_SYN_FILTER_SYNQFP;
		else
			synqf &= ~IXGBE_SYN_FILTER_SYNQFP;
	} else {
		if (!(synqf & IXGBE_SYN_FILTER_ENABLE))
			return -ENOENT;
		synqf &= ~(IXGBE_SYN_FILTER_QUEUE | IXGBE_SYN_FILTER_ENABLE);
	}
	IXGBE_WRITE_REG(hw, IXGBE_SYNQF, synqf);
	IXGBE_WRITE_FLUSH(hw);
	return 0;
}

static int
ixgbe_syn_filter_get(struct rte_eth_dev *dev,
			struct rte_eth_syn_filter *filter)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t synqf = IXGBE_READ_REG(hw, IXGBE_SYNQF);

	if (synqf & IXGBE_SYN_FILTER_ENABLE) {
		filter->hig_pri = (synqf & IXGBE_SYN_FILTER_SYNQFP) ? 1 : 0;
		filter->queue = (uint16_t)((synqf & IXGBE_SYN_FILTER_QUEUE) >> 1);
		return 0;
	}
	return -ENOENT;
}

static int
ixgbe_syn_filter_handle(struct rte_eth_dev *dev,
			enum rte_filter_op filter_op,
			void *arg)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	int ret;

	MAC_TYPE_FILTER_SUP(hw->mac.type);

	if (filter_op == RTE_ETH_FILTER_NOP)
		return 0;

	if (arg == NULL) {
		PMD_DRV_LOG(ERR, "arg shouldn't be NULL for operation %u",
			    filter_op);
		return -EINVAL;
	}

	switch (filter_op) {
	case RTE_ETH_FILTER_ADD:
		ret = ixgbe_syn_filter_set(dev,
				(struct rte_eth_syn_filter *)arg,
				TRUE);
		break;
	case RTE_ETH_FILTER_DELETE:
		ret = ixgbe_syn_filter_set(dev,
				(struct rte_eth_syn_filter *)arg,
				FALSE);
		break;
	case RTE_ETH_FILTER_GET:
		ret = ixgbe_syn_filter_get(dev,
				(struct rte_eth_syn_filter *)arg);
		break;
	default:
		PMD_DRV_LOG(ERR, "unsupported operation %u\n", filter_op);
		ret = -EINVAL;
		break;
	}

	return ret;
}


static inline enum ixgbe_5tuple_protocol
convert_protocol_type(uint8_t protocol_value)
{
	if (protocol_value == IPPROTO_TCP)
		return IXGBE_FILTER_PROTOCOL_TCP;
	else if (protocol_value == IPPROTO_UDP)
		return IXGBE_FILTER_PROTOCOL_UDP;
	else if (protocol_value == IPPROTO_SCTP)
		return IXGBE_FILTER_PROTOCOL_SCTP;
	else
		return IXGBE_FILTER_PROTOCOL_NONE;
}

/*
 * add a 5tuple filter
 *
 * @param
 * dev: Pointer to struct rte_eth_dev.
 * index: the index the filter allocates.
 * filter: ponter to the filter that will be added.
 * rx_queue: the queue id the filter assigned to.
 *
 * @return
 *    - On success, zero.
 *    - On failure, a negative value.
 */
static int
ixgbe_add_5tuple_filter(struct rte_eth_dev *dev,
			struct ixgbe_5tuple_filter *filter)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_filter_info *filter_info =
		IXGBE_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	int i, idx, shift;
	uint32_t ftqf, sdpqf;
	uint32_t l34timir = 0;
	uint8_t mask = 0xff;

	/*
	 * look for an unused 5tuple filter index,
	 * and insert the filter to list.
	 */
	for (i = 0; i < IXGBE_MAX_FTQF_FILTERS; i++) {
		idx = i / (sizeof(uint32_t) * NBBY);
		shift = i % (sizeof(uint32_t) * NBBY);
		if (!(filter_info->fivetuple_mask[idx] & (1 << shift))) {
			filter_info->fivetuple_mask[idx] |= 1 << shift;
			filter->index = i;
			TAILQ_INSERT_TAIL(&filter_info->fivetuple_list,
					  filter,
					  entries);
			break;
		}
	}
	if (i >= IXGBE_MAX_FTQF_FILTERS) {
		PMD_DRV_LOG(ERR, "5tuple filters are full.");
		return -ENOSYS;
	}

	sdpqf = (uint32_t)(filter->filter_info.dst_port <<
				IXGBE_SDPQF_DSTPORT_SHIFT);
	sdpqf = sdpqf | (filter->filter_info.src_port & IXGBE_SDPQF_SRCPORT);

	ftqf = (uint32_t)(filter->filter_info.proto &
		IXGBE_FTQF_PROTOCOL_MASK);
	ftqf |= (uint32_t)((filter->filter_info.priority &
		IXGBE_FTQF_PRIORITY_MASK) << IXGBE_FTQF_PRIORITY_SHIFT);
	if (filter->filter_info.src_ip_mask == 0) /* 0 means compare. */
		mask &= IXGBE_FTQF_SOURCE_ADDR_MASK;
	if (filter->filter_info.dst_ip_mask == 0)
		mask &= IXGBE_FTQF_DEST_ADDR_MASK;
	if (filter->filter_info.src_port_mask == 0)
		mask &= IXGBE_FTQF_SOURCE_PORT_MASK;
	if (filter->filter_info.dst_port_mask == 0)
		mask &= IXGBE_FTQF_DEST_PORT_MASK;
	if (filter->filter_info.proto_mask == 0)
		mask &= IXGBE_FTQF_PROTOCOL_COMP_MASK;
	ftqf |= mask << IXGBE_FTQF_5TUPLE_MASK_SHIFT;
	ftqf |= IXGBE_FTQF_POOL_MASK_EN;
	ftqf |= IXGBE_FTQF_QUEUE_ENABLE;

	IXGBE_WRITE_REG(hw, IXGBE_DAQF(i), filter->filter_info.dst_ip);
	IXGBE_WRITE_REG(hw, IXGBE_SAQF(i), filter->filter_info.src_ip);
	IXGBE_WRITE_REG(hw, IXGBE_SDPQF(i), sdpqf);
	IXGBE_WRITE_REG(hw, IXGBE_FTQF(i), ftqf);

	l34timir |= IXGBE_L34T_IMIR_RESERVE;
	l34timir |= (uint32_t)(filter->queue <<
				IXGBE_L34T_IMIR_QUEUE_SHIFT);
	IXGBE_WRITE_REG(hw, IXGBE_L34T_IMIR(i), l34timir);
	return 0;
}

/*
 * remove a 5tuple filter
 *
 * @param
 * dev: Pointer to struct rte_eth_dev.
 * filter: the pointer of the filter will be removed.
 */
static void
ixgbe_remove_5tuple_filter(struct rte_eth_dev *dev,
			struct ixgbe_5tuple_filter *filter)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_filter_info *filter_info =
		IXGBE_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	uint16_t index = filter->index;

	filter_info->fivetuple_mask[index / (sizeof(uint32_t) * NBBY)] &=
				~(1 << (index % (sizeof(uint32_t) * NBBY)));
	TAILQ_REMOVE(&filter_info->fivetuple_list, filter, entries);
	rte_free(filter);

	IXGBE_WRITE_REG(hw, IXGBE_DAQF(index), 0);
	IXGBE_WRITE_REG(hw, IXGBE_SAQF(index), 0);
	IXGBE_WRITE_REG(hw, IXGBE_SDPQF(index), 0);
	IXGBE_WRITE_REG(hw, IXGBE_FTQF(index), 0);
	IXGBE_WRITE_REG(hw, IXGBE_L34T_IMIR(index), 0);
}

static int
ixgbevf_dev_set_mtu(struct rte_eth_dev *dev, uint16_t mtu)
{
	struct ixgbe_hw *hw;
	uint32_t max_frame = mtu + ETHER_HDR_LEN + ETHER_CRC_LEN;

	hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	if ((mtu < ETHER_MIN_MTU) || (max_frame > ETHER_MAX_JUMBO_FRAME_LEN))
		return -EINVAL;

	/* refuse mtu that requires the support of scattered packets when this
	 * feature has not been enabled before. */
	if (!dev->data->scattered_rx &&
	    (max_frame + 2 * IXGBE_VLAN_TAG_SIZE >
	     dev->data->min_rx_buf_size - RTE_PKTMBUF_HEADROOM))
		return -EINVAL;

	/*
	 * When supported by the underlying PF driver, use the IXGBE_VF_SET_MTU
	 * request of the version 2.0 of the mailbox API.
	 * For now, use the IXGBE_VF_SET_LPE request of the version 1.0
	 * of the mailbox API.
	 * This call to IXGBE_SET_LPE action won't work with ixgbe pf drivers
	 * prior to 3.11.33 which contains the following change:
	 * "ixgbe: Enable jumbo frames support w/ SR-IOV"
	 */
	ixgbevf_rlpml_set_vf(hw, max_frame);

	/* update max frame size */
	dev->data->dev_conf.rxmode.max_rx_pkt_len = max_frame;
	return 0;
}

#define MAC_TYPE_FILTER_SUP_EXT(type)    do {\
	if ((type) != ixgbe_mac_82599EB && (type) != ixgbe_mac_X540)\
		return -ENOTSUP;\
} while (0)

static inline struct ixgbe_5tuple_filter *
ixgbe_5tuple_filter_lookup(struct ixgbe_5tuple_filter_list *filter_list,
			struct ixgbe_5tuple_filter_info *key)
{
	struct ixgbe_5tuple_filter *it;

	TAILQ_FOREACH(it, filter_list, entries) {
		if (memcmp(key, &it->filter_info,
			sizeof(struct ixgbe_5tuple_filter_info)) == 0) {
			return it;
		}
	}
	return NULL;
}

/* translate elements in struct rte_eth_ntuple_filter to struct ixgbe_5tuple_filter_info*/
static inline int
ntuple_filter_to_5tuple(struct rte_eth_ntuple_filter *filter,
			struct ixgbe_5tuple_filter_info *filter_info)
{
	if (filter->queue >= IXGBE_MAX_RX_QUEUE_NUM ||
		filter->priority > IXGBE_5TUPLE_MAX_PRI ||
		filter->priority < IXGBE_5TUPLE_MIN_PRI)
		return -EINVAL;

	switch (filter->dst_ip_mask) {
	case UINT32_MAX:
		filter_info->dst_ip_mask = 0;
		filter_info->dst_ip = filter->dst_ip;
		break;
	case 0:
		filter_info->dst_ip_mask = 1;
		break;
	default:
		PMD_DRV_LOG(ERR, "invalid dst_ip mask.");
		return -EINVAL;
	}

	switch (filter->src_ip_mask) {
	case UINT32_MAX:
		filter_info->src_ip_mask = 0;
		filter_info->src_ip = filter->src_ip;
		break;
	case 0:
		filter_info->src_ip_mask = 1;
		break;
	default:
		PMD_DRV_LOG(ERR, "invalid src_ip mask.");
		return -EINVAL;
	}

	switch (filter->dst_port_mask) {
	case UINT16_MAX:
		filter_info->dst_port_mask = 0;
		filter_info->dst_port = filter->dst_port;
		break;
	case 0:
		filter_info->dst_port_mask = 1;
		break;
	default:
		PMD_DRV_LOG(ERR, "invalid dst_port mask.");
		return -EINVAL;
	}

	switch (filter->src_port_mask) {
	case UINT16_MAX:
		filter_info->src_port_mask = 0;
		filter_info->src_port = filter->src_port;
		break;
	case 0:
		filter_info->src_port_mask = 1;
		break;
	default:
		PMD_DRV_LOG(ERR, "invalid src_port mask.");
		return -EINVAL;
	}

	switch (filter->proto_mask) {
	case UINT8_MAX:
		filter_info->proto_mask = 0;
		filter_info->proto =
			convert_protocol_type(filter->proto);
		break;
	case 0:
		filter_info->proto_mask = 1;
		break;
	default:
		PMD_DRV_LOG(ERR, "invalid protocol mask.");
		return -EINVAL;
	}

	filter_info->priority = (uint8_t)filter->priority;
	return 0;
}

/*
 * add or delete a ntuple filter
 *
 * @param
 * dev: Pointer to struct rte_eth_dev.
 * ntuple_filter: Pointer to struct rte_eth_ntuple_filter
 * add: if true, add filter, if false, remove filter
 *
 * @return
 *    - On success, zero.
 *    - On failure, a negative value.
 */
static int
ixgbe_add_del_ntuple_filter(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *ntuple_filter,
			bool add)
{
	struct ixgbe_filter_info *filter_info =
		IXGBE_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	struct ixgbe_5tuple_filter_info filter_5tuple;
	struct ixgbe_5tuple_filter *filter;
	int ret;

	if (ntuple_filter->flags != RTE_5TUPLE_FLAGS) {
		PMD_DRV_LOG(ERR, "only 5tuple is supported.");
		return -EINVAL;
	}

	memset(&filter_5tuple, 0, sizeof(struct ixgbe_5tuple_filter_info));
	ret = ntuple_filter_to_5tuple(ntuple_filter, &filter_5tuple);
	if (ret < 0)
		return ret;

	filter = ixgbe_5tuple_filter_lookup(&filter_info->fivetuple_list,
					 &filter_5tuple);
	if (filter != NULL && add) {
		PMD_DRV_LOG(ERR, "filter exists.");
		return -EEXIST;
	}
	if (filter == NULL && !add) {
		PMD_DRV_LOG(ERR, "filter doesn't exist.");
		return -ENOENT;
	}

	if (add) {
		filter = rte_zmalloc("ixgbe_5tuple_filter",
				sizeof(struct ixgbe_5tuple_filter), 0);
		if (filter == NULL)
			return -ENOMEM;
		(void)rte_memcpy(&filter->filter_info,
				 &filter_5tuple,
				 sizeof(struct ixgbe_5tuple_filter_info));
		filter->queue = ntuple_filter->queue;
		ret = ixgbe_add_5tuple_filter(dev, filter);
		if (ret < 0) {
			rte_free(filter);
			return ret;
		}
	} else
		ixgbe_remove_5tuple_filter(dev, filter);

	return 0;
}

/*
 * get a ntuple filter
 *
 * @param
 * dev: Pointer to struct rte_eth_dev.
 * ntuple_filter: Pointer to struct rte_eth_ntuple_filter
 *
 * @return
 *    - On success, zero.
 *    - On failure, a negative value.
 */
static int
ixgbe_get_ntuple_filter(struct rte_eth_dev *dev,
			struct rte_eth_ntuple_filter *ntuple_filter)
{
	struct ixgbe_filter_info *filter_info =
		IXGBE_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	struct ixgbe_5tuple_filter_info filter_5tuple;
	struct ixgbe_5tuple_filter *filter;
	int ret;

	if (ntuple_filter->flags != RTE_5TUPLE_FLAGS) {
		PMD_DRV_LOG(ERR, "only 5tuple is supported.");
		return -EINVAL;
	}

	memset(&filter_5tuple, 0, sizeof(struct ixgbe_5tuple_filter_info));
	ret = ntuple_filter_to_5tuple(ntuple_filter, &filter_5tuple);
	if (ret < 0)
		return ret;

	filter = ixgbe_5tuple_filter_lookup(&filter_info->fivetuple_list,
					 &filter_5tuple);
	if (filter == NULL) {
		PMD_DRV_LOG(ERR, "filter doesn't exist.");
		return -ENOENT;
	}
	ntuple_filter->queue = filter->queue;
	return 0;
}

/*
 * ixgbe_ntuple_filter_handle - Handle operations for ntuple filter.
 * @dev: pointer to rte_eth_dev structure
 * @filter_op:operation will be taken.
 * @arg: a pointer to specific structure corresponding to the filter_op
 *
 * @return
 *    - On success, zero.
 *    - On failure, a negative value.
 */
static int
ixgbe_ntuple_filter_handle(struct rte_eth_dev *dev,
				enum rte_filter_op filter_op,
				void *arg)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	int ret;

	MAC_TYPE_FILTER_SUP_EXT(hw->mac.type);

	if (filter_op == RTE_ETH_FILTER_NOP)
		return 0;

	if (arg == NULL) {
		PMD_DRV_LOG(ERR, "arg shouldn't be NULL for operation %u.",
			    filter_op);
		return -EINVAL;
	}

	switch (filter_op) {
	case RTE_ETH_FILTER_ADD:
		ret = ixgbe_add_del_ntuple_filter(dev,
			(struct rte_eth_ntuple_filter *)arg,
			TRUE);
		break;
	case RTE_ETH_FILTER_DELETE:
		ret = ixgbe_add_del_ntuple_filter(dev,
			(struct rte_eth_ntuple_filter *)arg,
			FALSE);
		break;
	case RTE_ETH_FILTER_GET:
		ret = ixgbe_get_ntuple_filter(dev,
			(struct rte_eth_ntuple_filter *)arg);
		break;
	default:
		PMD_DRV_LOG(ERR, "unsupported operation %u.", filter_op);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static inline int
ixgbe_ethertype_filter_lookup(struct ixgbe_filter_info *filter_info,
			uint16_t ethertype)
{
	int i;

	for (i = 0; i < IXGBE_MAX_ETQF_FILTERS; i++) {
		if (filter_info->ethertype_filters[i] == ethertype &&
		    (filter_info->ethertype_mask & (1 << i)))
			return i;
	}
	return -1;
}

static inline int
ixgbe_ethertype_filter_insert(struct ixgbe_filter_info *filter_info,
			uint16_t ethertype)
{
	int i;

	for (i = 0; i < IXGBE_MAX_ETQF_FILTERS; i++) {
		if (!(filter_info->ethertype_mask & (1 << i))) {
			filter_info->ethertype_mask |= 1 << i;
			filter_info->ethertype_filters[i] = ethertype;
			return i;
		}
	}
	return -1;
}

static inline int
ixgbe_ethertype_filter_remove(struct ixgbe_filter_info *filter_info,
			uint8_t idx)
{
	if (idx >= IXGBE_MAX_ETQF_FILTERS)
		return -1;
	filter_info->ethertype_mask &= ~(1 << idx);
	filter_info->ethertype_filters[idx] = 0;
	return idx;
}

static int
ixgbe_add_del_ethertype_filter(struct rte_eth_dev *dev,
			struct rte_eth_ethertype_filter *filter,
			bool add)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_filter_info *filter_info =
		IXGBE_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	uint32_t etqf = 0;
	uint32_t etqs = 0;
	int ret;

	if (filter->queue >= IXGBE_MAX_RX_QUEUE_NUM)
		return -EINVAL;

	if (filter->ether_type == ETHER_TYPE_IPv4 ||
		filter->ether_type == ETHER_TYPE_IPv6) {
		PMD_DRV_LOG(ERR, "unsupported ether_type(0x%04x) in"
			" ethertype filter.", filter->ether_type);
		return -EINVAL;
	}

	if (filter->flags & RTE_ETHTYPE_FLAGS_MAC) {
		PMD_DRV_LOG(ERR, "mac compare is unsupported.");
		return -EINVAL;
	}
	if (filter->flags & RTE_ETHTYPE_FLAGS_DROP) {
		PMD_DRV_LOG(ERR, "drop option is unsupported.");
		return -EINVAL;
	}

	ret = ixgbe_ethertype_filter_lookup(filter_info, filter->ether_type);
	if (ret >= 0 && add) {
		PMD_DRV_LOG(ERR, "ethertype (0x%04x) filter exists.",
			    filter->ether_type);
		return -EEXIST;
	}
	if (ret < 0 && !add) {
		PMD_DRV_LOG(ERR, "ethertype (0x%04x) filter doesn't exist.",
			    filter->ether_type);
		return -ENOENT;
	}

	if (add) {
		ret = ixgbe_ethertype_filter_insert(filter_info,
			filter->ether_type);
		if (ret < 0) {
			PMD_DRV_LOG(ERR, "ethertype filters are full.");
			return -ENOSYS;
		}
		etqf = IXGBE_ETQF_FILTER_EN;
		etqf |= (uint32_t)filter->ether_type;
		etqs |= (uint32_t)((filter->queue <<
				    IXGBE_ETQS_RX_QUEUE_SHIFT) &
				    IXGBE_ETQS_RX_QUEUE);
		etqs |= IXGBE_ETQS_QUEUE_EN;
	} else {
		ret = ixgbe_ethertype_filter_remove(filter_info, (uint8_t)ret);
		if (ret < 0)
			return -ENOSYS;
	}
	IXGBE_WRITE_REG(hw, IXGBE_ETQF(ret), etqf);
	IXGBE_WRITE_REG(hw, IXGBE_ETQS(ret), etqs);
	IXGBE_WRITE_FLUSH(hw);

	return 0;
}

static int
ixgbe_get_ethertype_filter(struct rte_eth_dev *dev,
			struct rte_eth_ethertype_filter *filter)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_filter_info *filter_info =
		IXGBE_DEV_PRIVATE_TO_FILTER_INFO(dev->data->dev_private);
	uint32_t etqf, etqs;
	int ret;

	ret = ixgbe_ethertype_filter_lookup(filter_info, filter->ether_type);
	if (ret < 0) {
		PMD_DRV_LOG(ERR, "ethertype (0x%04x) filter doesn't exist.",
			    filter->ether_type);
		return -ENOENT;
	}

	etqf = IXGBE_READ_REG(hw, IXGBE_ETQF(ret));
	if (etqf & IXGBE_ETQF_FILTER_EN) {
		etqs = IXGBE_READ_REG(hw, IXGBE_ETQS(ret));
		filter->ether_type = etqf & IXGBE_ETQF_ETHERTYPE;
		filter->flags = 0;
		filter->queue = (etqs & IXGBE_ETQS_RX_QUEUE) >>
			       IXGBE_ETQS_RX_QUEUE_SHIFT;
		return 0;
	}
	return -ENOENT;
}

/*
 * ixgbe_ethertype_filter_handle - Handle operations for ethertype filter.
 * @dev: pointer to rte_eth_dev structure
 * @filter_op:operation will be taken.
 * @arg: a pointer to specific structure corresponding to the filter_op
 */
static int
ixgbe_ethertype_filter_handle(struct rte_eth_dev *dev,
				enum rte_filter_op filter_op,
				void *arg)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	int ret;

	MAC_TYPE_FILTER_SUP(hw->mac.type);

	if (filter_op == RTE_ETH_FILTER_NOP)
		return 0;

	if (arg == NULL) {
		PMD_DRV_LOG(ERR, "arg shouldn't be NULL for operation %u.",
			    filter_op);
		return -EINVAL;
	}

	switch (filter_op) {
	case RTE_ETH_FILTER_ADD:
		ret = ixgbe_add_del_ethertype_filter(dev,
			(struct rte_eth_ethertype_filter *)arg,
			TRUE);
		break;
	case RTE_ETH_FILTER_DELETE:
		ret = ixgbe_add_del_ethertype_filter(dev,
			(struct rte_eth_ethertype_filter *)arg,
			FALSE);
		break;
	case RTE_ETH_FILTER_GET:
		ret = ixgbe_get_ethertype_filter(dev,
			(struct rte_eth_ethertype_filter *)arg);
		break;
	default:
		PMD_DRV_LOG(ERR, "unsupported operation %u.", filter_op);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int
ixgbe_dev_filter_ctrl(struct rte_eth_dev *dev,
		     enum rte_filter_type filter_type,
		     enum rte_filter_op filter_op,
		     void *arg)
{
	int ret = -EINVAL;

	switch (filter_type) {
	case RTE_ETH_FILTER_NTUPLE:
		ret = ixgbe_ntuple_filter_handle(dev, filter_op, arg);
		break;
	case RTE_ETH_FILTER_ETHERTYPE:
		ret = ixgbe_ethertype_filter_handle(dev, filter_op, arg);
		break;
	case RTE_ETH_FILTER_SYN:
		ret = ixgbe_syn_filter_handle(dev, filter_op, arg);
		break;
	case RTE_ETH_FILTER_FDIR:
		ret = ixgbe_fdir_ctrl_func(dev, filter_op, arg);
		break;
	default:
		PMD_DRV_LOG(WARNING, "Filter type (%d) not supported",
							filter_type);
		break;
	}

	return ret;
}

static u8 *
ixgbe_dev_addr_list_itr(__attribute__((unused)) struct ixgbe_hw *hw,
			u8 **mc_addr_ptr, u32 *vmdq)
{
	u8 *mc_addr;

	*vmdq = 0;
	mc_addr = *mc_addr_ptr;
	*mc_addr_ptr = (mc_addr + sizeof(struct ether_addr));
	return mc_addr;
}

static int
ixgbe_dev_set_mc_addr_list(struct rte_eth_dev *dev,
			  struct ether_addr *mc_addr_set,
			  uint32_t nb_mc_addr)
{
	struct ixgbe_hw *hw;
	u8 *mc_addr_list;

	hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	mc_addr_list = (u8 *)mc_addr_set;
	return ixgbe_update_mc_addr_list(hw, mc_addr_list, nb_mc_addr,
					 ixgbe_dev_addr_list_itr, TRUE);
}

static uint64_t
ixgbe_read_systime_cyclecounter(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint64_t systime_cycles;

	switch (hw->mac.type) {
	case ixgbe_mac_X550:
		/* SYSTIMEL stores ns and SYSTIMEH stores seconds. */
		systime_cycles = (uint64_t)IXGBE_READ_REG(hw, IXGBE_SYSTIML);
		systime_cycles += (uint64_t)IXGBE_READ_REG(hw, IXGBE_SYSTIMH)
				* NSEC_PER_SEC;
		break;
	default:
		systime_cycles = (uint64_t)IXGBE_READ_REG(hw, IXGBE_SYSTIML);
		systime_cycles |= (uint64_t)IXGBE_READ_REG(hw, IXGBE_SYSTIMH)
				<< 32;
	}

	return systime_cycles;
}

static uint64_t
ixgbe_read_rx_tstamp_cyclecounter(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint64_t rx_tstamp_cycles;

	switch (hw->mac.type) {
	case ixgbe_mac_X550:
		/* RXSTMPL stores ns and RXSTMPH stores seconds. */
		rx_tstamp_cycles = (uint64_t)IXGBE_READ_REG(hw, IXGBE_RXSTMPL);
		rx_tstamp_cycles += (uint64_t)IXGBE_READ_REG(hw, IXGBE_RXSTMPH)
				* NSEC_PER_SEC;
		break;
	default:
		/* RXSTMPL stores ns and RXSTMPH stores seconds. */
		rx_tstamp_cycles = (uint64_t)IXGBE_READ_REG(hw, IXGBE_RXSTMPL);
		rx_tstamp_cycles |= (uint64_t)IXGBE_READ_REG(hw, IXGBE_RXSTMPH)
				<< 32;
	}

	return rx_tstamp_cycles;
}

static uint64_t
ixgbe_read_tx_tstamp_cyclecounter(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint64_t tx_tstamp_cycles;

	switch (hw->mac.type) {
	case ixgbe_mac_X550:
		/* TXSTMPL stores ns and TXSTMPH stores seconds. */
		tx_tstamp_cycles = (uint64_t)IXGBE_READ_REG(hw, IXGBE_TXSTMPL);
		tx_tstamp_cycles += (uint64_t)IXGBE_READ_REG(hw, IXGBE_TXSTMPH)
				* NSEC_PER_SEC;
		break;
	default:
		/* TXSTMPL stores ns and TXSTMPH stores seconds. */
		tx_tstamp_cycles = (uint64_t)IXGBE_READ_REG(hw, IXGBE_TXSTMPL);
		tx_tstamp_cycles |= (uint64_t)IXGBE_READ_REG(hw, IXGBE_TXSTMPH)
				<< 32;
	}

	return tx_tstamp_cycles;
}

static void
ixgbe_start_timecounters(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_adapter *adapter =
		(struct ixgbe_adapter *)dev->data->dev_private;
	struct rte_eth_link link;
	uint32_t incval = 0;
	uint32_t shift = 0;

	/* Get current link speed. */
	memset(&link, 0, sizeof(link));
	ixgbe_dev_link_update(dev, 1);
	rte_ixgbe_dev_atomic_read_link_status(dev, &link);

	switch (link.link_speed) {
	case ETH_LINK_SPEED_100:
		incval = IXGBE_INCVAL_100;
		shift = IXGBE_INCVAL_SHIFT_100;
		break;
	case ETH_LINK_SPEED_1000:
		incval = IXGBE_INCVAL_1GB;
		shift = IXGBE_INCVAL_SHIFT_1GB;
		break;
	case ETH_LINK_SPEED_10000:
	default:
		incval = IXGBE_INCVAL_10GB;
		shift = IXGBE_INCVAL_SHIFT_10GB;
		break;
	}

	switch (hw->mac.type) {
	case ixgbe_mac_X550:
		/* Independent of link speed. */
		incval = 1;
		/* Cycles read will be interpreted as ns. */
		shift = 0;
		/* Fall-through */
	case ixgbe_mac_X540:
		IXGBE_WRITE_REG(hw, IXGBE_TIMINCA, incval);
		break;
	case ixgbe_mac_82599EB:
		incval >>= IXGBE_INCVAL_SHIFT_82599;
		shift -= IXGBE_INCVAL_SHIFT_82599;
		IXGBE_WRITE_REG(hw, IXGBE_TIMINCA,
				(1 << IXGBE_INCPER_SHIFT_82599) | incval);
		break;
	default:
		/* Not supported. */
		return;
	}

	memset(&adapter->systime_tc, 0, sizeof(struct rte_timecounter));
	memset(&adapter->rx_tstamp_tc, 0, sizeof(struct rte_timecounter));
	memset(&adapter->tx_tstamp_tc, 0, sizeof(struct rte_timecounter));

	adapter->systime_tc.cc_mask = IXGBE_CYCLECOUNTER_MASK;
	adapter->systime_tc.cc_shift = shift;
	adapter->systime_tc.nsec_mask = (1ULL << shift) - 1;

	adapter->rx_tstamp_tc.cc_mask = IXGBE_CYCLECOUNTER_MASK;
	adapter->rx_tstamp_tc.cc_shift = shift;
	adapter->rx_tstamp_tc.nsec_mask = (1ULL << shift) - 1;

	adapter->tx_tstamp_tc.cc_mask = IXGBE_CYCLECOUNTER_MASK;
	adapter->tx_tstamp_tc.cc_shift = shift;
	adapter->tx_tstamp_tc.nsec_mask = (1ULL << shift) - 1;
}

static int
ixgbe_timesync_adjust_time(struct rte_eth_dev *dev, int64_t delta)
{
	struct ixgbe_adapter *adapter =
			(struct ixgbe_adapter *)dev->data->dev_private;

	adapter->systime_tc.nsec += delta;
	adapter->rx_tstamp_tc.nsec += delta;
	adapter->tx_tstamp_tc.nsec += delta;

	return 0;
}

static int
ixgbe_timesync_write_time(struct rte_eth_dev *dev, const struct timespec *ts)
{
	uint64_t ns;
	struct ixgbe_adapter *adapter =
			(struct ixgbe_adapter *)dev->data->dev_private;

	ns = rte_timespec_to_ns(ts);
	/* Set the timecounters to a new value. */
	adapter->systime_tc.nsec = ns;
	adapter->rx_tstamp_tc.nsec = ns;
	adapter->tx_tstamp_tc.nsec = ns;

	return 0;
}

static int
ixgbe_timesync_read_time(struct rte_eth_dev *dev, struct timespec *ts)
{
	uint64_t ns, systime_cycles;
	struct ixgbe_adapter *adapter =
			(struct ixgbe_adapter *)dev->data->dev_private;

	systime_cycles = ixgbe_read_systime_cyclecounter(dev);
	ns = rte_timecounter_update(&adapter->systime_tc, systime_cycles);
	*ts = rte_ns_to_timespec(ns);

	return 0;
}

static int
ixgbe_timesync_enable(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t tsync_ctl;
	uint32_t tsauxc;

	/* Stop the timesync system time. */
	IXGBE_WRITE_REG(hw, IXGBE_TIMINCA, 0x0);
	/* Reset the timesync system time value. */
	IXGBE_WRITE_REG(hw, IXGBE_SYSTIML, 0x0);
	IXGBE_WRITE_REG(hw, IXGBE_SYSTIMH, 0x0);

	/* Enable system time for platforms where it isn't on by default. */
	tsauxc = IXGBE_READ_REG(hw, IXGBE_TSAUXC);
	tsauxc &= ~IXGBE_TSAUXC_DISABLE_SYSTIME;
	IXGBE_WRITE_REG(hw, IXGBE_TSAUXC, tsauxc);

	ixgbe_start_timecounters(dev);

	/* Enable L2 filtering of IEEE1588/802.1AS Ethernet frame types. */
	IXGBE_WRITE_REG(hw, IXGBE_ETQF(IXGBE_ETQF_FILTER_1588),
			(ETHER_TYPE_1588 |
			 IXGBE_ETQF_FILTER_EN |
			 IXGBE_ETQF_1588));

	/* Enable timestamping of received PTP packets. */
	tsync_ctl = IXGBE_READ_REG(hw, IXGBE_TSYNCRXCTL);
	tsync_ctl |= IXGBE_TSYNCRXCTL_ENABLED;
	IXGBE_WRITE_REG(hw, IXGBE_TSYNCRXCTL, tsync_ctl);

	/* Enable timestamping of transmitted PTP packets. */
	tsync_ctl = IXGBE_READ_REG(hw, IXGBE_TSYNCTXCTL);
	tsync_ctl |= IXGBE_TSYNCTXCTL_ENABLED;
	IXGBE_WRITE_REG(hw, IXGBE_TSYNCTXCTL, tsync_ctl);

	IXGBE_WRITE_FLUSH(hw);

	return 0;
}

static int
ixgbe_timesync_disable(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t tsync_ctl;

	/* Disable timestamping of transmitted PTP packets. */
	tsync_ctl = IXGBE_READ_REG(hw, IXGBE_TSYNCTXCTL);
	tsync_ctl &= ~IXGBE_TSYNCTXCTL_ENABLED;
	IXGBE_WRITE_REG(hw, IXGBE_TSYNCTXCTL, tsync_ctl);

	/* Disable timestamping of received PTP packets. */
	tsync_ctl = IXGBE_READ_REG(hw, IXGBE_TSYNCRXCTL);
	tsync_ctl &= ~IXGBE_TSYNCRXCTL_ENABLED;
	IXGBE_WRITE_REG(hw, IXGBE_TSYNCRXCTL, tsync_ctl);

	/* Disable L2 filtering of IEEE1588/802.1AS Ethernet frame types. */
	IXGBE_WRITE_REG(hw, IXGBE_ETQF(IXGBE_ETQF_FILTER_1588), 0);

	/* Stop incrementating the System Time registers. */
	IXGBE_WRITE_REG(hw, IXGBE_TIMINCA, 0);

	return 0;
}

static int
ixgbe_timesync_read_rx_timestamp(struct rte_eth_dev *dev,
				 struct timespec *timestamp,
				 uint32_t flags __rte_unused)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_adapter *adapter =
		(struct ixgbe_adapter *)dev->data->dev_private;
	uint32_t tsync_rxctl;
	uint64_t rx_tstamp_cycles;
	uint64_t ns;

	tsync_rxctl = IXGBE_READ_REG(hw, IXGBE_TSYNCRXCTL);
	if ((tsync_rxctl & IXGBE_TSYNCRXCTL_VALID) == 0)
		return -EINVAL;

	rx_tstamp_cycles = ixgbe_read_rx_tstamp_cyclecounter(dev);
	ns = rte_timecounter_update(&adapter->rx_tstamp_tc, rx_tstamp_cycles);
	*timestamp = rte_ns_to_timespec(ns);

	return  0;
}

static int
ixgbe_timesync_read_tx_timestamp(struct rte_eth_dev *dev,
				 struct timespec *timestamp)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_adapter *adapter =
		(struct ixgbe_adapter *)dev->data->dev_private;
	uint32_t tsync_txctl;
	uint64_t tx_tstamp_cycles;
	uint64_t ns;

	tsync_txctl = IXGBE_READ_REG(hw, IXGBE_TSYNCTXCTL);
	if ((tsync_txctl & IXGBE_TSYNCTXCTL_VALID) == 0)
		return -EINVAL;

	tx_tstamp_cycles = ixgbe_read_tx_tstamp_cyclecounter(dev);
	ns = rte_timecounter_update(&adapter->tx_tstamp_tc, tx_tstamp_cycles);
	*timestamp = rte_ns_to_timespec(ns);

	return 0;
}

static int
ixgbe_get_reg_length(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	int count = 0;
	int g_ind = 0;
	const struct reg_info *reg_group;
	const struct reg_info **reg_set = (hw->mac.type == ixgbe_mac_82598EB) ?
				    ixgbe_regs_mac_82598EB : ixgbe_regs_others;

	while ((reg_group = reg_set[g_ind++]))
		count += ixgbe_regs_group_count(reg_group);

	return count;
}

static int
ixgbevf_get_reg_length(struct rte_eth_dev *dev __rte_unused)
{
	int count = 0;
	int g_ind = 0;
	const struct reg_info *reg_group;

	while ((reg_group = ixgbevf_regs[g_ind++]))
		count += ixgbe_regs_group_count(reg_group);

	return count;
}

static int
ixgbe_get_regs(struct rte_eth_dev *dev,
	      struct rte_dev_reg_info *regs)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t *data = regs->data;
	int g_ind = 0;
	int count = 0;
	const struct reg_info *reg_group;
	const struct reg_info **reg_set = (hw->mac.type == ixgbe_mac_82598EB) ?
				    ixgbe_regs_mac_82598EB : ixgbe_regs_others;

	/* Support only full register dump */
	if ((regs->length == 0) ||
	    (regs->length == (uint32_t)ixgbe_get_reg_length(dev))) {
		regs->version = hw->mac.type << 24 | hw->revision_id << 16 |
			hw->device_id;
		while ((reg_group = reg_set[g_ind++]))
			count += ixgbe_read_regs_group(dev, &data[count],
				reg_group);
		return 0;
	}

	return -ENOTSUP;
}

static int
ixgbevf_get_regs(struct rte_eth_dev *dev,
		struct rte_dev_reg_info *regs)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint32_t *data = regs->data;
	int g_ind = 0;
	int count = 0;
	const struct reg_info *reg_group;

	/* Support only full register dump */
	if ((regs->length == 0) ||
	    (regs->length == (uint32_t)ixgbevf_get_reg_length(dev))) {
		regs->version = hw->mac.type << 24 | hw->revision_id << 16 |
			hw->device_id;
		while ((reg_group = ixgbevf_regs[g_ind++]))
			count += ixgbe_read_regs_group(dev, &data[count],
						      reg_group);
		return 0;
	}

	return -ENOTSUP;
}

static int
ixgbe_get_eeprom_length(struct rte_eth_dev *dev)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	/* Return unit is byte count */
	return hw->eeprom.word_size * 2;
}

static int
ixgbe_get_eeprom(struct rte_eth_dev *dev,
		struct rte_dev_eeprom_info *in_eeprom)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_eeprom_info *eeprom = &hw->eeprom;
	uint16_t *data = in_eeprom->data;
	int first, length;

	first = in_eeprom->offset >> 1;
	length = in_eeprom->length >> 1;
	if ((first > hw->eeprom.word_size) ||
	    ((first + length) > hw->eeprom.word_size))
		return -EINVAL;

	in_eeprom->magic = hw->vendor_id | (hw->device_id << 16);

	return eeprom->ops.read_buffer(hw, first, length, data);
}

static int
ixgbe_set_eeprom(struct rte_eth_dev *dev,
		struct rte_dev_eeprom_info *in_eeprom)
{
	struct ixgbe_hw *hw = IXGBE_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct ixgbe_eeprom_info *eeprom = &hw->eeprom;
	uint16_t *data = in_eeprom->data;
	int first, length;

	first = in_eeprom->offset >> 1;
	length = in_eeprom->length >> 1;
	if ((first > hw->eeprom.word_size) ||
	    ((first + length) > hw->eeprom.word_size))
		return -EINVAL;

	in_eeprom->magic = hw->vendor_id | (hw->device_id << 16);

	return eeprom->ops.write_buffer(hw,  first, length, data);
}

uint16_t
ixgbe_reta_size_get(enum ixgbe_mac_type mac_type) {
	switch (mac_type) {
	case ixgbe_mac_X550:
	case ixgbe_mac_X550EM_x:
		return ETH_RSS_RETA_SIZE_512;
	case ixgbe_mac_X550_vf:
	case ixgbe_mac_X550EM_x_vf:
		return ETH_RSS_RETA_SIZE_64;
	default:
		return ETH_RSS_RETA_SIZE_128;
	}
}

uint32_t
ixgbe_reta_reg_get(enum ixgbe_mac_type mac_type, uint16_t reta_idx) {
	switch (mac_type) {
	case ixgbe_mac_X550:
	case ixgbe_mac_X550EM_x:
		if (reta_idx < ETH_RSS_RETA_SIZE_128)
			return IXGBE_RETA(reta_idx >> 2);
		else
			return IXGBE_ERETA((reta_idx - ETH_RSS_RETA_SIZE_128) >> 2);
	case ixgbe_mac_X550_vf:
	case ixgbe_mac_X550EM_x_vf:
		return IXGBE_VFRETA(reta_idx >> 2);
	default:
		return IXGBE_RETA(reta_idx >> 2);
	}
}

uint32_t
ixgbe_mrqc_reg_get(enum ixgbe_mac_type mac_type) {
	switch (mac_type) {
	case ixgbe_mac_X550_vf:
	case ixgbe_mac_X550EM_x_vf:
		return IXGBE_VFMRQC;
	default:
		return IXGBE_MRQC;
	}
}

uint32_t
ixgbe_rssrk_reg_get(enum ixgbe_mac_type mac_type, uint8_t i) {
	switch (mac_type) {
	case ixgbe_mac_X550_vf:
	case ixgbe_mac_X550EM_x_vf:
		return IXGBE_VFRSSRK(i);
	default:
		return IXGBE_RSSRK(i);
	}
}

bool
ixgbe_rss_update_sp(enum ixgbe_mac_type mac_type) {
	switch (mac_type) {
	case ixgbe_mac_82599_vf:
	case ixgbe_mac_X540_vf:
		return 0;
	default:
		return 1;
	}
}

static int
ixgbe_dev_get_dcb_info(struct rte_eth_dev *dev,
			struct rte_eth_dcb_info *dcb_info)
{
	struct ixgbe_dcb_config *dcb_config =
			IXGBE_DEV_PRIVATE_TO_DCB_CFG(dev->data->dev_private);
	struct ixgbe_dcb_tc_config *tc;
	uint8_t i, j;

	if (dev->data->dev_conf.rxmode.mq_mode & ETH_MQ_RX_DCB_FLAG)
		dcb_info->nb_tcs = dcb_config->num_tcs.pg_tcs;
	else
		dcb_info->nb_tcs = 1;

	if (dcb_config->vt_mode) { /* vt is enabled*/
		struct rte_eth_vmdq_dcb_conf *vmdq_rx_conf =
				&dev->data->dev_conf.rx_adv_conf.vmdq_dcb_conf;
		for (i = 0; i < ETH_DCB_NUM_USER_PRIORITIES; i++)
			dcb_info->prio_tc[i] = vmdq_rx_conf->dcb_tc[i];
		for (i = 0; i < vmdq_rx_conf->nb_queue_pools; i++) {
			for (j = 0; j < dcb_info->nb_tcs; j++) {
				dcb_info->tc_queue.tc_rxq[i][j].base =
						i * dcb_info->nb_tcs + j;
				dcb_info->tc_queue.tc_rxq[i][j].nb_queue = 1;
				dcb_info->tc_queue.tc_txq[i][j].base =
						i * dcb_info->nb_tcs + j;
				dcb_info->tc_queue.tc_txq[i][j].nb_queue = 1;
			}
		}
	} else { /* vt is disabled*/
		struct rte_eth_dcb_rx_conf *rx_conf =
				&dev->data->dev_conf.rx_adv_conf.dcb_rx_conf;
		for (i = 0; i < ETH_DCB_NUM_USER_PRIORITIES; i++)
			dcb_info->prio_tc[i] = rx_conf->dcb_tc[i];
		if (dcb_info->nb_tcs == ETH_4_TCS) {
			for (i = 0; i < dcb_info->nb_tcs; i++) {
				dcb_info->tc_queue.tc_rxq[0][i].base = i * 32;
				dcb_info->tc_queue.tc_rxq[0][i].nb_queue = 16;
			}
			dcb_info->tc_queue.tc_txq[0][0].base = 0;
			dcb_info->tc_queue.tc_txq[0][1].base = 64;
			dcb_info->tc_queue.tc_txq[0][2].base = 96;
			dcb_info->tc_queue.tc_txq[0][3].base = 112;
			dcb_info->tc_queue.tc_txq[0][0].nb_queue = 64;
			dcb_info->tc_queue.tc_txq[0][1].nb_queue = 32;
			dcb_info->tc_queue.tc_txq[0][2].nb_queue = 16;
			dcb_info->tc_queue.tc_txq[0][3].nb_queue = 16;
		} else if (dcb_info->nb_tcs == ETH_8_TCS) {
			for (i = 0; i < dcb_info->nb_tcs; i++) {
				dcb_info->tc_queue.tc_rxq[0][i].base = i * 16;
				dcb_info->tc_queue.tc_rxq[0][i].nb_queue = 16;
			}
			dcb_info->tc_queue.tc_txq[0][0].base = 0;
			dcb_info->tc_queue.tc_txq[0][1].base = 32;
			dcb_info->tc_queue.tc_txq[0][2].base = 64;
			dcb_info->tc_queue.tc_txq[0][3].base = 80;
			dcb_info->tc_queue.tc_txq[0][4].base = 96;
			dcb_info->tc_queue.tc_txq[0][5].base = 104;
			dcb_info->tc_queue.tc_txq[0][6].base = 112;
			dcb_info->tc_queue.tc_txq[0][7].base = 120;
			dcb_info->tc_queue.tc_txq[0][0].nb_queue = 32;
			dcb_info->tc_queue.tc_txq[0][1].nb_queue = 32;
			dcb_info->tc_queue.tc_txq[0][2].nb_queue = 16;
			dcb_info->tc_queue.tc_txq[0][3].nb_queue = 16;
			dcb_info->tc_queue.tc_txq[0][4].nb_queue = 8;
			dcb_info->tc_queue.tc_txq[0][5].nb_queue = 8;
			dcb_info->tc_queue.tc_txq[0][6].nb_queue = 8;
			dcb_info->tc_queue.tc_txq[0][7].nb_queue = 8;
		}
	}
	for (i = 0; i < dcb_info->nb_tcs; i++) {
		tc = &dcb_config->tc_config[i];
		dcb_info->tc_bws[i] = tc->path[IXGBE_DCB_TX_CONFIG].bwg_percent;
	}
	return 0;
}

static struct rte_driver rte_ixgbe_driver = {
	.type = PMD_PDEV,
	.init = rte_ixgbe_pmd_init,
};

static struct rte_driver rte_ixgbevf_driver = {
	.type = PMD_PDEV,
	.init = rte_ixgbevf_pmd_init,
};

PMD_REGISTER_DRIVER(rte_ixgbe_driver);
PMD_REGISTER_DRIVER(rte_ixgbevf_driver);
