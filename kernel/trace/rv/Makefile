# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I $(src)		# needed for trace events

obj-$(CONFIG_RV) += rv.o
obj-$(CONFIG_RV_MON_WIP) += monitors/wip/wip.o
obj-$(CONFIG_RV_MON_WWNR) += monitors/wwnr/wwnr.o
# Add new monitors here
obj-$(CONFIG_RV_REACTORS) += rv_reactors.o
obj-$(CONFIG_RV_REACT_PRINTK) += reactor_printk.o
obj-$(CONFIG_RV_REACT_PANIC) += reactor_panic.o
