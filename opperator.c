/*
 opperator.ko - The OPP Mannagement API
 version 0.1-beta1 - 11-30-11
 by Jeffrey Kawika Patricio <jkp@tekahuna.net>
 License: GNU GPLv3
 <http://www.gnu.org/licenses/gpl-3.0.html>
 
 Project site:
 http://code.google.com/p/opperator/
 
 Changelog:
 
 version 0.1-beta1 - 11-11-11
 - Initilize git repository.
 - Cleaned up code to work on OMAP2+ w/kernel 2.6.35-7 and greater, not just OMAP4
 - Misc Cleanup
 
 version 0.1-alpha - 11-11-11
 - Initial working build.
*/
 
#include <linux/module.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <asm/uaccess.h>
#include <linux/cpufreq.h>
#include <plat/common.h>
#include <plat/voltage.h>


#include "opp_info_razr.h"
#include "../symsearch/symsearch.h"

#define DRIVER_AUTHOR "Jeffrey Kawika Patricio <jkp@tekahuna.net>\n"
#define DRIVER_DESCRIPTION "opperator.ko - The OPP Management API\n Note: This module makes use of \
							SYMSEARCH by Skrilax_CZ & is inspired\n by Milestone Overclock by Tiago Sousa\n"
#define DRIVER_VERSION "0.1-beta1"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

// opp.c
SYMSEARCH_DECLARE_FUNCTION_STATIC(int, opp_get_opp_count_fp, struct device *dev);
SYMSEARCH_DECLARE_FUNCTION_STATIC(struct omap_opp *, opp_find_freq_floor_fp, struct device *dev, unsigned long *freq);
//SYMSEARCH_DECLARE_FUNCTION_STATIC(struct device_opp *, find_device_opp_fp, struct device *dev);
// voltage.c
SYMSEARCH_DECLARE_FUNCTION_STATIC(void, omap_voltage_reset_fp, struct voltagedomain *voltdm);
SYMSEARCH_DECLARE_FUNCTION_STATIC(unsigned long, omap_vp_get_curr_volt_fp, struct voltagedomain *voltdm);
SYMSEARCH_DECLARE_FUNCTION_STATIC(unsigned long, omap_voltage_get_nom_volt_fp, struct voltagedomain *voltdm);
SYMSEARCH_DECLARE_FUNCTION_STATIC(struct voltagedomain *, omap_voltage_domain_get_fp, char *name);


static int maxdex;
static unsigned long default_max_rate;
static unsigned long default_max_voltage;

static struct omap_opp *default_mpu_opps;
static struct omap_vdd_info *default_mpu_vdd_info;
static struct cpufreq_frequency_table *freq_table;
static struct cpufreq_policy *policy;

#define BUF_SIZE PAGE_SIZE
static char *buf;

static int proc_opperator_read(char *buffer, char **buffer_location,
							   off_t offset, int count, int *eof, void *data)
{
	int i, ret = 0;
	unsigned long freq = ULONG_MAX;
	struct device *dev = NULL;
	struct voltagedomain *voltdm = NULL;
	struct omap_vdd_info *vdd;
	struct omap_opp *opp = ERR_PTR(-ENODEV);
	
	voltdm = omap_voltage_domain_get_fp("mpu");
	if (!voltdm || IS_ERR(voltdm)) {
		pr_warning("%s: VDD specified does not exist!\n", __func__);
		return -EINVAL;
	}
	vdd = container_of(voltdm, struct omap_vdd_info, voltdm);

	ret += scnprintf(buffer+ret, count-ret, "mpu: volt_data_count=%u\n", vdd->volt_data_count);
	for (i = 0; i < vdd->volt_data_count; i++) {
		ret += scnprintf(buffer+ret, count-ret, "mpu: volt_data=%u\n", vdd->volt_data[i].volt_nominal);
	}
	for (i = 0;vdd->dep_vdd_info[0].dep_table[i].main_vdd_volt != 0; i++) {
		ret += scnprintf(buffer+ret, count-ret, "mpu: main_vdd_volt=%u dep_vdd_volt=%u\n", 
						 vdd->dep_vdd_info[0].dep_table[i].main_vdd_volt,
						 vdd->dep_vdd_info[0].dep_table[i].dep_vdd_volt);
	}
	dev = omap2_get_mpuss_device();
	if (IS_ERR(dev)) {
		return -ENODEV;
	}
	opp = opp_find_freq_floor_fp(dev, &freq);
	if (IS_ERR(opp)) {
		return -ENODEV;
	}
	ret += scnprintf(buffer+ret, count-ret, "mpu: opp_count=%u\n", opp->dev_opp->opp_count);
	ret += scnprintf(buffer+ret, count-ret, "mpu: opp_count_enabled=%u\n", opp->dev_opp->enabled_opp_count);
	ret += scnprintf(buffer+ret, count-ret, "mpu: default_max_rate=%lu\n", default_max_rate);
	ret += scnprintf(buffer+ret, count-ret, "mpu: default_max_voltage=%lu\n", default_max_voltage);
	ret += scnprintf(buffer+ret, count-ret, "mpu: current_voltdm_voltage=%lu\n", omap_vp_get_curr_volt_fp(voltdm));
	ret += scnprintf(buffer+ret, count-ret, "mpu: nominal_voltdm_voltage=%lu\n", omap_voltage_get_nom_volt_fp(voltdm));
	freq = ULONG_MAX;
	while (!IS_ERR(opp = opp_find_freq_floor_fp(dev, &freq))) {
		ret += scnprintf(buffer+ret, count-ret, "mpu: enabled=%u rate=%lu voltage=%lu\n", 
											 opp->enabled, opp->rate, opp->u_volt);
		freq--;
	}
	return ret;
};

static int proc_opperator_write(struct file *filp, const char __user *buffer,
								unsigned long len, void *data)
{
	unsigned long volt, rate, freq = ULONG_MAX;
	struct device *dev = NULL;
	struct voltagedomain *voltdm = NULL;
	struct omap_vdd_info *vdd;
	struct omap_opp *opp = ERR_PTR(-ENODEV);
	
	if(!len || len >= BUF_SIZE)
		return -ENOSPC;
	if(copy_from_user(buf, buffer, len))
		return -EFAULT;
	buf[len] = 0;
	if(sscanf(buf, "%lu %lu", &rate, &volt) == 2) {
		voltdm = omap_voltage_domain_get_fp("mpu");
		if (!voltdm || IS_ERR(voltdm)) {
			pr_warning("%s: VDD specified does not exist!\n", __func__);
			return -EINVAL;
		}
		vdd = container_of(voltdm, struct omap_vdd_info, voltdm);
		mutex_lock(&vdd->scaling_mutex);
		dev = omap2_get_mpuss_device();
		opp = opp_find_freq_floor_fp(dev, &freq);
		if (IS_ERR(opp)) {
			return -ENODEV;
		}
		freq_table[0].frequency = policy->min = policy->cpuinfo.min_freq =
		policy->user_policy.min = opp->rate / 1000;
		vdd->volt_data[maxdex+1].volt_nominal = volt;
		vdd->dep_vdd_info[0].dep_table[maxdex+1].main_vdd_volt = volt;
		opp->u_volt = volt;
		opp->rate = rate;
		omap_voltage_reset_fp(voltdm);
		freq_table[maxdex].frequency = policy->max = policy->cpuinfo.max_freq =
			policy->user_policy.max = rate / 1000;
		freq_table[0].frequency = policy->min = policy->cpuinfo.min_freq =
			policy->user_policy.min = 300000;
		mutex_unlock(&vdd->scaling_mutex);
	} else
		printk(KERN_INFO "OPPerator: incorrect parameters\n");
	return len;
};
							 
static int __init opperator_init(void)
{
	unsigned long freq = ULONG_MAX;
	struct device *dev = NULL;
	struct omap_opp *opp = ERR_PTR(-ENODEV);
	struct proc_dir_entry *proc_entry;
	
	printk(KERN_INFO " %s Version: %s\n", DRIVER_DESCRIPTION, DRIVER_VERSION);
	printk(KERN_INFO " Created by: %s\n", DRIVER_AUTHOR);

	// opp.c
	SYMSEARCH_BIND_FUNCTION_TO(opperator, opp_get_opp_count, opp_get_opp_count_fp);
	SYMSEARCH_BIND_FUNCTION_TO(opperator, opp_find_freq_floor, opp_find_freq_floor_fp);
//	SYMSEARCH_BIND_FUNCTION_TO(opperator, find_device_opp, find_device_opp_fp);
	//voltage.c
	SYMSEARCH_BIND_FUNCTION_TO(opperator, omap_voltage_reset, omap_voltage_reset_fp);
	SYMSEARCH_BIND_FUNCTION_TO(opperator, omap_vp_get_curr_volt, omap_vp_get_curr_volt_fp);
	SYMSEARCH_BIND_FUNCTION_TO(opperator, omap_voltage_get_nom_volt, omap_voltage_get_nom_volt_fp);
	SYMSEARCH_BIND_FUNCTION_TO(opperator, omap_voltage_domain_get, omap_voltage_domain_get_fp);
	
	freq_table = cpufreq_frequency_get_table(0);
	policy = cpufreq_cpu_get(0);
	
	dev = omap2_get_mpuss_device();
	maxdex = (opp_get_opp_count_fp(dev)-1);
	opp = opp_find_freq_floor_fp(dev, &freq);
	if (IS_ERR(opp)) {
		return -ENODEV;
	}
	default_max_rate = opp->rate;
	default_max_voltage = opp->u_volt;
	
	buf = (char *)vmalloc(BUF_SIZE);
	
	proc_entry = create_proc_read_entry("opperator", 0644, NULL, proc_opperator_read, NULL);
	proc_entry->write_proc = proc_opperator_write;
	
	return 0;
};

static void __exit opperator_exit(void)
{
	unsigned long freq = ULONG_MAX;
	struct device *dev = NULL;
	struct omap_opp *opp = ERR_PTR(-ENODEV);
	
	remove_proc_entry("opperator", NULL);
	
	vfree(buf);
	
	dev = omap2_get_mpuss_device();
	opp = opp_find_freq_floor_fp(dev, &freq);
	opp->rate = default_max_rate;
	opp->u_volt = default_max_voltage;
	freq_table[maxdex].frequency = policy->max = policy->cpuinfo.max_freq =
	policy->user_policy.max = default_max_rate / 1000;
	
	printk(KERN_INFO " OPPerator: Reseting values to default... Goodbye!\n");
};
							 
module_init(opperator_init);
module_exit(opperator_exit);

