/*
 * servoblaster.c Multiple Servo Driver for the RaspberryPi
 * Copyright (c) 2012 Richard Hirst <richardghirst@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * If you want the device node created automatically create these two
 * files, and make /lib/udev/servoblaster executable (chmod +x):
 *
 * ============= /etc/udev/rules.d/20-servoblaster.rules =============
 * SUBSYSTEM=="module", DEVPATH=="/module/servoblaster", RUN+="/lib/udev/servoblaster"
 * ===================================================================
 *
 * ===================== /lib/udev/servoblaster ======================
 * #!/bin/bash
 *
 * if [ "$ACTION" = "remove" ]; then
 *         rm -f /dev/servoblaster
 * elif [ "$ACTION" = "add" ]; then
 *          major=$( sed -n 's/ servoblaster//p' /proc/devices )
 *        [ "$major" ] && mknod -m 0666 /dev/servoblaster c $major 0
 * fi
 *
 * exit 0
 * ===================================================================
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/cdev.h>
#include <linux/scatterlist.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <mach/platform.h>
#include <asm/uaccess.h>
#include <mach/dma.h>
//#include "servoblaster.h"

#define GPIO_LEN		0xb4
#define DMA_LEN			0x24
#define PWM_BASE		(BCM2708_PERI_BASE + 0x20C000)
#define PWM_LEN			0x28
#define CLK_BASE        (BCM2708_PERI_BASE + 0x101000)
#define CLK_LEN			0xA8

#define GPFSEL0			(0x00/4)
#define GPFSEL1			(0x04/4)
#define GPSET0			(0x1c/4)
#define GPCLR0			(0x28/4)

#define PWM_CTL			(0x00/4)
#define PWM_DMAC		(0x08/4)
#define PWM_RNG1		(0x10/4)
#define PWM_FIFO		(0x18/4)

#define PWMCLK_CNTL		40
#define PWMCLK_DIV		41

#define PWMCTL_MODE1	(1<<1)
#define PWMCTL_PWEN1	(1<<0)
#define PWMCTL_CLRF		(1<<6)
#define PWMCTL_USEF1	(1<<5)

#define PWMDMAC_ENAB	(1<<31)
// I think this means it requests as soon as there is one free slot in the FIFO
// which is what we want as burst DMA would mess up our timing..
#define PWMDMAC_THRSHLD	((15<<8)|(15<<0))

#define DMA_CS			(BCM2708_DMA_CS/4)
#define DMA_CONBLK_AD	(BCM2708_DMA_ADDR/4)
#define DMA_DEBUG		(BCM2708_DMA_DEBUG/4)

#define BCM2708_DMA_END				(1<<1)	// Why is this not in mach/dma.h ?
#define BCM2708_DMA_NO_WIDE_BURSTS	(1<<26)

static int dev_open(struct inode *, struct file *);
static int dev_close(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static long dev_ioctl(struct file *, unsigned int, unsigned long);

static struct file_operations fops = 
{
	.open = dev_open,
	.read = dev_read,
	.write = dev_write,
	.release = dev_close,
	.unlocked_ioctl = dev_ioctl,
	.compat_ioctl = dev_ioctl,
};

// Map servo channels to GPIO pins
static uint8_t servo2gpio[] = {
		4,	// P1-7
		17,	// P1-11
#ifdef PWM0_ON_GPIO18
		1,	// P1-5 (GPIO-18, P1-12 is currently PWM0, for debug)
#else
		18,	// P1-12
#endif
		21,	// P1-13
		22,	// P1-15
		23,	// P1-16
		24,	// P1-18
		25,	// P1-22
};

#define MAX_SERVOS	(sizeof(servo2gpio)/sizeof(servo2gpio[0]))

// Structure of our control data, stored in a 4K page, and accessed by dma controller
struct ctldata_s {
	struct bcm2708_dma_cb cb[MAX_SERVOS * 4];	// gpio-hi, delay, gpio-lo, delay, for each servo output
	uint32_t gpiodata[MAX_SERVOS];				// set-pin, clear-pin values, per servo output
	uint32_t pwmdata;							// the word we write to the pwm fifo
};

static struct ctldata_s *ctl;
static unsigned long ctldatabase;
static volatile uint32_t *gpio_reg;
static volatile uint32_t *dma_reg;
static volatile uint32_t *clk_reg;
static volatile uint32_t *pwm_reg;

static dev_t devno;
static struct cdev my_cdev;
static int my_major;
static int cycle_ticks = 2000;
static int tick_scale = 6;
static int num_servos = MAX_SERVOS;

// Wait until we're not processing the given servo (actually wait until
// we are not processing the low period of the previous servo, or the
// high period of this one).
static int wait_for_servo(int servo)
{
	local_irq_disable();
	for (;;) {
		int cb = (dma_reg[DMA_CONBLK_AD] - ((uint32_t)ctl->cb & 0x7fffffff)) / sizeof(ctl->cb[0]);

		if (servo > 0) {
			if (cb < servo*4-2 || cb > servo*4+2)
				break;
		} else {
			if (cb > 2 && cb < num_servos*4-2)
				break;
		}
		local_irq_enable();
		set_current_state(TASK_INTERRUPTIBLE);
		if (schedule_timeout(1))
			return -EINTR;
		local_irq_disable();
	}
	// Return with IRQs disabled!!!
	return 0;
}

int init_module(void)
{
	int res, i, s;

	if (num_servos < 1 || num_servos > 8) {
		printk(KERN_WARNING "ServoBlaster: invalid num_servos argument specified. Valid values are 1-8.\n");
		return -EINVAL;
	}
	
	res = alloc_chrdev_region(&devno, 0, 1, "servoblaster");
	if (res < 0) {
		printk(KERN_WARNING "ServoBlaster: Can't allocated device number\n");
		return res;
	}
	my_major = MAJOR(devno);
	cdev_init(&my_cdev, &fops);
	my_cdev.owner = THIS_MODULE;
	my_cdev.ops = &fops;
	res = cdev_add(&my_cdev, MKDEV(my_major, 0), 1);
	if (res) {
		printk(KERN_WARNING "ServoBlaster: Error %d adding device\n", res);
		unregister_chrdev_region(devno, 1);
		return res;
	}

	ctldatabase = __get_free_pages(GFP_KERNEL, 0);
	printk(KERN_INFO "ServoBlaster: Control page is at 0x%lx, cycle_ticks %d, tick_scale %d, num_servos %d\n", 
			ctldatabase, cycle_ticks, tick_scale, num_servos);
	if (ctldatabase == 0) {
		printk(KERN_WARNING "ServoBlaster: alloc_pages failed\n");
		cdev_del(&my_cdev);
		unregister_chrdev_region(devno, 1);
		return -EFAULT;
	}
	ctl = (struct ctldata_s *)ctldatabase;
	gpio_reg = (uint32_t *)ioremap(GPIO_BASE, GPIO_LEN);
	dma_reg  = (uint32_t *)ioremap(DMA_BASE,  DMA_LEN);
	clk_reg  = (uint32_t *)ioremap(CLK_BASE,  CLK_LEN);
	pwm_reg  = (uint32_t *)ioremap(PWM_BASE,  PWM_LEN);

	memset(ctl, 0, sizeof(*ctl));

	// Set all servo control pins to be outputs
	for (i = 0; i < num_servos; i++) {
		int gpio = servo2gpio[i];
		int fnreg = gpio/10 + GPFSEL0;
		int fnshft = (gpio %10) * 3;
		gpio_reg[GPCLR0] = 1 << gpio;
		gpio_reg[fnreg] = (gpio_reg[fnreg] & ~(7 << fnshft)) | (1 << fnshft);
	}
#ifdef PWM0_ON_GPIO18
	// Set pwm0 output on gpio18
	gpio_reg[GPCLR0] = 1 << 18;
	gpio_reg[GPFSEL1] = (gpio_reg[GPFSEL1] & ~(7 << 8*3)) | ( 2 << 8*3);
#endif

	// Build the DMA CB chain
	for (s = 0; s < num_servos; s++) {
		int i = s*4;
		// Set gpio high
		ctl->gpiodata[s] = 1 << servo2gpio[s];
		ctl->cb[i].info   = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP;
		ctl->cb[i].src    = (uint32_t)(&ctl->gpiodata[s]) & 0x7fffffff;
		// We clear the GPIO here initially, so outputs go to 0 on startup
		// Once someone writes to /dev/servoblaster we'll patch it to a 'set'
		ctl->cb[i].dst    = ((GPIO_BASE + GPCLR0*4) & 0x00ffffff) | 0x7e000000;
		ctl->cb[i].length = sizeof(uint32_t);
		ctl->cb[i].stride = 0;
		ctl->cb[i].next = (uint32_t)(ctl->cb + i + 1) & 0x7fffffff;
		// delay
		i++;
		ctl->cb[i].info   = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP | BCM2708_DMA_D_DREQ | BCM2708_DMA_PER_MAP(5);
		ctl->cb[i].src    = (uint32_t)(&ctl->pwmdata) & 0x7fffffff;
		ctl->cb[i].dst    = ((PWM_BASE + PWM_FIFO*4) & 0x00ffffff) | 0x7e000000;
		ctl->cb[i].length = sizeof(uint32_t) * 1;	// default 1 tick high
		ctl->cb[i].stride = 0;
		ctl->cb[i].next = (uint32_t)(ctl->cb + i + 1) & 0x7fffffff;
		// Set gpio lo
		i++;
		ctl->cb[i].info   = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP;
		ctl->cb[i].src    = (uint32_t)(&ctl->gpiodata[s]) & 0x7fffffff;
		ctl->cb[i].dst    = ((GPIO_BASE + GPCLR0*4) & 0x00ffffff) | 0x7e000000;
		ctl->cb[i].length = sizeof(uint32_t);
		ctl->cb[i].stride = 0;
		ctl->cb[i].next = (uint32_t)(ctl->cb + i + 1) & 0x7fffffff;
		// delay
		i++;
		ctl->cb[i].info   = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP | BCM2708_DMA_D_DREQ | BCM2708_DMA_PER_MAP(5);
		ctl->cb[i].src    = (uint32_t)(&ctl->pwmdata) & 0x7fffffff;
		ctl->cb[i].dst    = ((PWM_BASE + PWM_FIFO*4) & 0x00ffffff) | 0x7e000000;
		ctl->cb[i].length = sizeof(uint32_t) * (cycle_ticks / num_servos - 1);	
		//ctl->cb[i].length = sizeof(uint32_t) * (cycle_ticks / 8 - 1);
		ctl->cb[i].stride = 0;
		ctl->cb[i].next = (uint32_t)(ctl->cb + i + 1) & 0x7fffffff;
	}
	// Point last cb back to first one so it loops continuously
	ctl->cb[num_servos*4-1].next = (uint32_t)(ctl->cb) & 0x7fffffff;

	// Initialise PWM
	pwm_reg[PWM_CTL] = 0;
	udelay(10);
	clk_reg[PWMCLK_DIV] = 0x5A000000 | (32<<12);    // set pwm div to 32 (19.2MHz/32 = 600KHz)
	clk_reg[PWMCLK_CNTL] = 0x5A000011;              // Source=osc and enable
	pwm_reg[PWM_RNG1] = tick_scale;							// 600KHz/6 = 10us per FIFO write
	udelay(10);
	ctl->pwmdata = 1;								// Give a pulse of one clock width for each fifo write
	pwm_reg[PWM_DMAC] = PWMDMAC_ENAB | PWMDMAC_THRSHLD;
	udelay(10);
	pwm_reg[PWM_CTL] = PWMCTL_CLRF;
	udelay(10);
	pwm_reg[PWM_CTL] = PWMCTL_USEF1 | PWMCTL_PWEN1;
	udelay(10);

	// Initialise the DMA
	dma_reg[DMA_CS] = BCM2708_DMA_RESET;
	udelay(10);
	dma_reg[DMA_CS] = BCM2708_DMA_INT | BCM2708_DMA_END;
	dma_reg[DMA_CONBLK_AD] = (uint32_t)(ctl->cb) & 0x7fffffff;
	dma_reg[DMA_DEBUG] = 7; // clear debug error flags
	dma_reg[DMA_CS] = 0x10880001;	// go, mid priority, wait for outstanding writes

	return 0;
}


void cleanup_module(void)
{
	int servo;

	// Take care to stop servos with outputs low, so we don't get
	// spurious movement on module unload
	for (servo = 0; servo < num_servos; servo++) {
		// Wait until we're not driving this servo
		if (wait_for_servo(servo))
			break;
		// Patch the control block so it stays low
		ctl->cb[servo*4+0].dst = ((GPIO_BASE + GPCLR0*4) & 0x00ffffff) | 0x7e000000;
		local_irq_enable();
	}
	// Wait 20ms to be sure it has finished it's cycle an all outputs are low
	msleep(20);
	// Now we can kill everything
	dma_reg[DMA_CS] = BCM2708_DMA_RESET;
	pwm_reg[PWM_CTL] = 0;
	udelay(10);
	free_pages(ctldatabase, 0);
	iounmap(gpio_reg);
	iounmap(dma_reg);
	iounmap(clk_reg);
	iounmap(pwm_reg);
	cdev_del(&my_cdev);
	unregister_chrdev_region(devno, 1);
}


static int dev_open(struct inode *inod,struct file *fil)
{
	return 0;
}

static ssize_t dev_read(struct file *filp,char *buf,size_t count,loff_t *f_pos)
{
	return 0;
}

static ssize_t dev_write(struct file *filp,const char *buf,size_t count,loff_t *f_pos)
{
	int servo;
	int cnt;
	int n;
	char str[32];
	char dummy;

	cnt = count < 32 ? count : 31;
	if (copy_from_user(str, buf, cnt)) {
		return -EFAULT;
	}
	str[cnt] = '\0';
	n = sscanf(str, "%d=%d\n%c", &servo, &cnt, &dummy);
	if (n != 2) {
		printk(KERN_WARNING "ServoBlaster: Failed to parse command (n=%d)\n", n);
		return -EINVAL;
	}
	if (servo < 0 || servo >= num_servos) {
		printk(KERN_WARNING "ServoBlaster: Bad servo number %d\n", servo);
		return -EINVAL;
	}
	if (cnt < 0 || cnt > cycle_ticks / num_servos - 1) {
		printk(KERN_WARNING "ServoBlaster: Bad value %d\n", cnt);
		return -EINVAL;
	}
	if (wait_for_servo(servo))
		return -EINTR;
	if (cnt == 0) {
		ctl->cb[servo*4+0].dst = ((GPIO_BASE + GPCLR0*4) & 0x00ffffff) | 0x7e000000;
	} else {
		ctl->cb[servo*4+0].dst = ((GPIO_BASE + GPSET0*4) & 0x00ffffff) | 0x7e000000;
		ctl->cb[servo*4+1].length = cnt * sizeof(uint32_t);
		ctl->cb[servo*4+3].length = (cycle_ticks / num_servos - cnt) * sizeof(uint32_t);
	}
	local_irq_enable();

	return count;
}

static int dev_close(struct inode *inod,struct file *fil)
{
	return 0;
}

static long dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return -EINVAL;
}

MODULE_DESCRIPTION("ServoBlaster, Multiple Servo Driver for the RaspberryPi");
MODULE_AUTHOR("Richard Hirst <richardghirst@gmail.com>");
MODULE_LICENSE("GPL v2");

module_param(cycle_ticks, int, 0);
MODULE_PARM_DESC(cycle_ticks, "number of ticks per cycle, max pulse is cycle_ticks/8");

module_param(tick_scale, int, 0);
MODULE_PARM_DESC(tick_scale, "scale the tick length, 6 should be 10us");

module_param(num_servos, int, 0);
MODULE_PARM_DESC(num_servos, "Number of servos (1-8) starting from servo 0 (default 8)");

