/*
 * lirc_openrex.c
 *
 * lirc_openrex  Device driver that records pulse- and pause-lengths
 *	      (space-lengths) (just like the lirc_serial driver does)
 *	      between GPIO interrupt events on the OpenRex.
 *            almost all code is taken from the lirc_rpi module by
 *            Aron Robert Szabo.   
 *
 * Copyright (C) 2013 Rene van den Braken <rvdbraken@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* OpenRex uses gpio2_io11 
   also known as SD4_DAT3 and if exported as /sys/class/gpio/gpio43 */
 
#define MYGPIO  IMX_GPIO_NR(2, 11)

#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <asm/io.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <media/lirc.h>
#include <media/lirc_dev.h>
#include <linux/platform_device.h>

#define LIRC_DRIVER_NAME "lirc_openrex"
#define RBUF_LEN 256

#define dprintk(fmt, args...)                                  \
       do {                                                    \
               if (debug)                                      \
                       printk(KERN_DEBUG LIRC_DRIVER_NAME ": " \
                              fmt, ## args);                   \
       } while (0)


static int debug = 1 ;
/* -1 = auto, 0 = active high, 1 = active low */
static int sense = -1;

unsigned int irq_num = -1;
unsigned int irq_counter = 0;

static struct platform_device *lirc_openrex_dev;
static struct timeval lasttv = { 0, 0 };
static struct lirc_buffer rbuf;
static spinlock_t lock;

struct irq_chip *irqchip;
struct irq_data *irqdata;

/* initialized/set in init_timing_params() */
static unsigned int freq = 38000;
static unsigned int duty_cycle = 50;
static unsigned long period;
static unsigned long pulse_width;
static unsigned long space_width;
/* forward declarations */
static void lirc_openrex_exit(void);

static int init_timing_params(unsigned int new_duty_cycle,
       unsigned int new_freq)
{
       /*
        * period, pulse/space width are kept with 8 binary places -
        * IE multiplied by 256.
        */
       duty_cycle = new_duty_cycle;
       freq = new_freq;
       period = 256 * 1000000L / freq;
       pulse_width = period * duty_cycle / 100;
       space_width = period - pulse_width;
       //dprintk("in init_timing_params, freq=%d pulse=%ld, "
       //       "space=%ld\n", freq, pulse_width, space_width);
       return 0;
}

static void rbwrite(int l)
{
       if (lirc_buffer_full(&rbuf)) {
               /* no new signals will be accepted */
               dprintk("Buffer overrun\n");
               return;
       }
       lirc_buffer_write(&rbuf, (void *)&l);
}

static void frbwrite(int l)
{
	/* simple noise filter */
	static int pulse, space;
	static unsigned int ptr;

	if (ptr > 0 && (l & PULSE_BIT)) {
		pulse += l & PULSE_MASK;
		if (pulse > 250) {
			rbwrite(space);
			rbwrite(pulse | PULSE_BIT);
			ptr = 0;
			pulse = 0;
		}
		return;
	}
	if (!(l & PULSE_BIT)) {
		if (ptr == 0) {
			if (l > 20000) {
				space = l;
				ptr++;
				return;
			}
		} else {
			if (l > 20000) {
				space += pulse;
				if (space > PULSE_MASK)
					space = PULSE_MASK;
				space += l;
				if (space > PULSE_MASK)
					space = PULSE_MASK;
				pulse = 0;
				return;
			}
			rbwrite(space);
			rbwrite(pulse | PULSE_BIT);
			ptr = 0;
			pulse = 0;
		}
	}
	rbwrite(l);
}

static irqreturn_t irq_handler(int i, void *blah, struct pt_regs *regs)
{
	struct timeval tv;
	long deltv;
	int data;
	int signal;

	/* use the GPIO signal level */
	signal = gpio_get_value(MYGPIO);

	/* unmask the irq */
	irqchip->irq_unmask(irqdata);

	if (sense != -1) {
		/* get current time */
		do_gettimeofday(&tv);

		/* calc time since last interrupt in microseconds */
		deltv = tv.tv_sec-lasttv.tv_sec;
		if (tv.tv_sec < lasttv.tv_sec ||
		    (tv.tv_sec == lasttv.tv_sec &&
		     tv.tv_usec < lasttv.tv_usec)) {
			printk(KERN_WARNING LIRC_DRIVER_NAME
			       ": AIEEEE: your clock just jumped backwards\n");
			printk(KERN_WARNING LIRC_DRIVER_NAME
			       ": %d %d %lx %lx %lx %lx\n", signal, sense,
			       tv.tv_sec, lasttv.tv_sec,
			       tv.tv_usec, lasttv.tv_usec);
			data = PULSE_MASK;
		} else if (deltv > 15) {
			data = PULSE_MASK; /* really long time */
			if (!(signal^sense)) {
				/* sanity check */
				printk(KERN_WARNING LIRC_DRIVER_NAME
				       ": AIEEEE: %d %d %lx %lx %lx %lx\n",
				       signal, sense, tv.tv_sec, lasttv.tv_sec,
				       tv.tv_usec, lasttv.tv_usec);
				/*
				 * detecting pulse while this
				 * MUST be a space!
				 */
				sense = sense ? 0 : 1;
			}
		} else {
			data = (int) (deltv*1000000 +
				      (tv.tv_usec - lasttv.tv_usec));
		}
		frbwrite(signal^sense ? data : (data|PULSE_BIT));
		lasttv = tv;
		wake_up_interruptible(&rbuf.wait_poll);
	}

	return IRQ_HANDLED;
}

static int init_port(void)
{
	int i, nlow, nhigh, ret, irq;

	if (gpio_request(MYGPIO, LIRC_DRIVER_NAME " ir/in")) {
		printk(KERN_ALERT LIRC_DRIVER_NAME
		       ": cant claim gpio pin %d\n", MYGPIO);
		ret = -ENODEV;
		goto exit_init_port ;
	}

	if( gpio_direction_input(MYGPIO) ) {
          printk( KERN_INFO "%s: cannot set gpio direction\n", "lirc_openrex" );
  	  goto exit_init_port ;
        }

        irq = gpio_to_irq( MYGPIO ); 
	dprintk("using irq %d\n", irq);
	irqdata = irq_get_irq_data(irq);

	if (irqdata && irqdata->chip) {
		irqchip = irqdata->chip;
	} else {
		ret = -ENODEV;
		goto exit_gpio_free_in_pin;
	}

	/* if pin is high, then this must be an active low receiver. */
	if (sense == -1) {
		/* wait 1/2 sec for the power supply */
		msleep(500);

		/*
		 * probe 9 times every 0.04s, collect "votes" for
		 * active high/low
		 */
		nlow = 0;
		nhigh = 0;
		for (i = 0; i < 9; i++) {
			if (gpio_get_value(MYGPIO))
				nlow++;
			else
				nhigh++;
			msleep(40);
		}
		sense = (nlow >= nhigh ? 1 : 0);
		printk(KERN_INFO LIRC_DRIVER_NAME
		       ": auto-detected active %s receiver on GPIO pin %d\n",
		       sense ? "low" : "high", MYGPIO);
	} else {
		printk(KERN_INFO LIRC_DRIVER_NAME
		       ": manually using active %s receiver on GPIO pin %d\n",
		       sense ? "low" : "high", MYGPIO);
	}

	return 0;

	exit_gpio_free_in_pin:
	gpio_free(MYGPIO);

	exit_init_port:
	return ret;
}

// called when the character device is opened
static int set_use_inc(void *data)
{
	int result;
	unsigned long flags;

	/* initialize timestamp */
	do_gettimeofday(&lasttv);

	result = request_irq(gpio_to_irq(MYGPIO),
			     (irq_handler_t) irq_handler, 0,
			     LIRC_DRIVER_NAME, (void*) 0);

	switch (result) {
	case -EBUSY:
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": IRQ %d is busy\n",
		       gpio_to_irq(MYGPIO));
		return -EBUSY;
	case -EINVAL:
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": Bad irq number or handler\n");
		return -EINVAL;
	default:
		dprintk("Interrupt %d obtained\n",
			gpio_to_irq(MYGPIO));
		break;
	};

	/* initialize pulse/space widths */
	init_timing_params(duty_cycle, freq);

	spin_lock_irqsave(&lock, flags);

	/* GPIO Pin Falling/Rising Edge Detect Enable */
	irqchip->irq_set_type(irqdata,
			      IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING);

	/* unmask the irq */
	irqchip->irq_unmask(irqdata);

	spin_unlock_irqrestore(&lock, flags);

	return 0;
}

static void set_use_dec(void *data)
{
       unsigned long flags;

       spin_lock_irqsave(&lock, flags);

       /* GPIO Pin Falling/Rising Edge Detect Disable */
       irqchip->irq_set_type(irqdata, 0);
       irqchip->irq_mask(irqdata);

       spin_unlock_irqrestore(&lock, flags);

       free_irq(gpio_to_irq( MYGPIO), (void *) 0);

       dprintk(KERN_INFO LIRC_DRIVER_NAME
               ": freed IRQ %d\n", gpio_to_irq( MYGPIO));
}

static const struct file_operations lirc_fops = {
       .owner          = THIS_MODULE,
       .unlocked_ioctl = lirc_dev_fop_ioctl,
       .read           = lirc_dev_fop_read,
       .poll           = lirc_dev_fop_poll,
       .open           = lirc_dev_fop_open,
       .release        = lirc_dev_fop_close,
       .llseek         = no_llseek,
};


static struct lirc_driver driver = {
       .name           = LIRC_DRIVER_NAME,
       .minor          = -1,
       .code_length    = 1,
       .sample_rate    = 0,
       .data           = NULL,
       .add_to_buf     = NULL,
       .rbuf           = &rbuf,
       .set_use_inc    = set_use_inc,
       .set_use_dec    = set_use_dec,
       .fops           = &lirc_fops,
       .dev            = NULL,
       .owner          = THIS_MODULE,
};

static struct platform_driver lirc_openrex_driver = {
       .driver = {
               .name   = LIRC_DRIVER_NAME,
               .owner  = THIS_MODULE,
       }
};

static int __init lirc_openrex_init(void)
{
	int result;

	/* Init read buffer. */
	result = lirc_buffer_init(&rbuf, sizeof(int), RBUF_LEN);
	if (result < 0)
		return -ENOMEM;

	result = platform_driver_register(&lirc_openrex_driver);
	if (result) {
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": lirc register returned %d\n", result);
		goto exit_buffer_free;
	}

	lirc_openrex_dev = platform_device_alloc(LIRC_DRIVER_NAME, 0);
	if (!lirc_openrex_dev) {
		result = -ENOMEM;
		goto exit_driver_unregister;
	}

	result = platform_device_add(lirc_openrex_dev);
	if (result)
		goto exit_device_put;

	return 0;

	exit_device_put:
	platform_device_put(lirc_openrex_dev);

	exit_driver_unregister:
	platform_driver_unregister(&lirc_openrex_driver);

	exit_buffer_free:
	lirc_buffer_free(&rbuf);

	return result;
}

static void lirc_openrex_exit(void)
{
       gpio_free(MYGPIO);
       platform_device_unregister(lirc_openrex_dev);
       platform_driver_unregister(&lirc_openrex_driver);
       lirc_buffer_free(&rbuf);
}


static int __init lirc_openrex_init_module(void)
{
	int result;

	result = lirc_openrex_init();
	if (result)
		return result;

	driver.features = LIRC_CAN_REC_MODE2;
	driver.dev = &lirc_openrex_dev->dev;
	driver.minor = lirc_register_driver(&driver);

	if (driver.minor < 0) {
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": device registration failed with %d\n", result);
		result = -EIO;
		goto exit_openrex;
	}

	printk(KERN_INFO LIRC_DRIVER_NAME ": driver registered!\n");

	result = init_port();
	if (result < 0)
		goto exit_openrex;

	return 0;

	exit_openrex:
	lirc_openrex_exit();

	return result;
}

static void __exit lirc_openrex_exit_module(void)
{
       lirc_openrex_exit();
       lirc_unregister_driver(driver.minor);
       printk(KERN_INFO LIRC_DRIVER_NAME ": cleaned up module\n");
}

module_init(lirc_openrex_init_module);
module_exit(lirc_openrex_exit_module);

MODULE_DESCRIPTION("Infra-red receiver driver for openrex GPIO.");
MODULE_AUTHOR("Rene van den Braken  <rvdbraken@gmail.com");
MODULE_AUTHOR("Aron Robert Szabo <aron@reon.hu>");
MODULE_AUTHOR("Michael Bishop <cleverca22@gmail.com>");
MODULE_LICENSE("GPL");
