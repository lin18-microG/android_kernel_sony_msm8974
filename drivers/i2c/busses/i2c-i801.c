/*
    Copyright (c) 1998 - 2002  Frodo Looijaard <frodol@dds.nl>,
    Philip Edelbrock <phil@netroedge.com>, and Mark D. Studebaker
    <mdsxyz123@yahoo.com>
    Copyright (C) 2007 - 2012  Jean Delvare <khali@linux-fr.org>
    Copyright (C) 2010         Intel Corporation,
                               David Woodhouse <dwmw2@infradead.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
  Supports the following Intel I/O Controller Hubs (ICH):

                                  I/O                     Block   I2C
                                  region  SMBus   Block   proc.   block
  Chip name             PCI ID    size    PEC     buffer  call    read
  ----------------------------------------------------------------------
  82801AA  (ICH)        0x2413     16      no      no      no      no
  82801AB  (ICH0)       0x2423     16      no      no      no      no
  82801BA  (ICH2)       0x2443     16      no      no      no      no
  82801CA  (ICH3)       0x2483     32     soft     no      no      no
  82801DB  (ICH4)       0x24c3     32     hard     yes     no      no
  82801E   (ICH5)       0x24d3     32     hard     yes     yes     yes
  6300ESB               0x25a4     32     hard     yes     yes     yes
  82801F   (ICH6)       0x266a     32     hard     yes     yes     yes
  6310ESB/6320ESB       0x269b     32     hard     yes     yes     yes
  82801G   (ICH7)       0x27da     32     hard     yes     yes     yes
  82801H   (ICH8)       0x283e     32     hard     yes     yes     yes
  82801I   (ICH9)       0x2930     32     hard     yes     yes     yes
  EP80579 (Tolapai)     0x5032     32     hard     yes     yes     yes
  ICH10                 0x3a30     32     hard     yes     yes     yes
  ICH10                 0x3a60     32     hard     yes     yes     yes
  5/3400 Series (PCH)   0x3b30     32     hard     yes     yes     yes
  6 Series (PCH)        0x1c22     32     hard     yes     yes     yes
  Patsburg (PCH)        0x1d22     32     hard     yes     yes     yes
  Patsburg (PCH) IDF    0x1d70     32     hard     yes     yes     yes
  Patsburg (PCH) IDF    0x1d71     32     hard     yes     yes     yes
  Patsburg (PCH) IDF    0x1d72     32     hard     yes     yes     yes
  DH89xxCC (PCH)        0x2330     32     hard     yes     yes     yes
  Panther Point (PCH)   0x1e22     32     hard     yes     yes     yes
  Lynx Point (PCH)      0x8c22     32     hard     yes     yes     yes
  Lynx Point-LP (PCH)   0x9c22     32     hard     yes     yes     yes
  Avoton (SOC)          0x1f3c     32     hard     yes     yes     yes

  Features supported by this driver:
  Software PEC                     no
  Hardware PEC                     yes
  Block buffer                     yes
  Block process call transaction   no
  I2C block read transaction       yes  (doesn't use the block buffer)
  Slave mode                       no

  See the file Documentation/i2c/busses/i2c-i801 for details.
*/

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/dmi.h>
#include <linux/slab.h>

/* I801 SMBus address offsets */
#define SMBHSTSTS(p)	(0 + (p)->smba)
#define SMBHSTCNT(p)	(2 + (p)->smba)
#define SMBHSTCMD(p)	(3 + (p)->smba)
#define SMBHSTADD(p)	(4 + (p)->smba)
#define SMBHSTDAT0(p)	(5 + (p)->smba)
#define SMBHSTDAT1(p)	(6 + (p)->smba)
#define SMBBLKDAT(p)	(7 + (p)->smba)
#define SMBPEC(p)	(8 + (p)->smba)		/* ICH3 and later */
#define SMBAUXSTS(p)	(12 + (p)->smba)	/* ICH4 and later */
#define SMBAUXCTL(p)	(13 + (p)->smba)	/* ICH4 and later */

/* PCI Address Constants */
#define SMBBAR		4
#define SMBHSTCFG	0x040

/* Host configuration bits for SMBHSTCFG */
#define SMBHSTCFG_HST_EN	1
#define SMBHSTCFG_SMB_SMI_EN	2
#define SMBHSTCFG_I2C_EN	4

/* Auxiliary control register bits, ICH4+ only */
#define SMBAUXCTL_CRC		1
#define SMBAUXCTL_E32B		2

/* kill bit for SMBHSTCNT */
#define SMBHSTCNT_KILL		2

/* Other settings */
#define MAX_RETRIES		400
#define ENABLE_INT9		0	/* set to 0x01 to enable - untested */

/* I801 command constants */
#define I801_QUICK		0x00
#define I801_BYTE		0x04
#define I801_BYTE_DATA		0x08
#define I801_WORD_DATA		0x0C
#define I801_PROC_CALL		0x10	/* unimplemented */
#define I801_BLOCK_DATA		0x14
#define I801_I2C_BLOCK_DATA	0x18	/* ICH5 and later */
#define I801_BLOCK_LAST		0x34
#define I801_I2C_BLOCK_LAST	0x38	/* ICH5 and later */
#define I801_START		0x40
#define I801_PEC_EN		0x80	/* ICH3 and later */

/* I801 Hosts Status register bits */
#define SMBHSTSTS_BYTE_DONE	0x80
#define SMBHSTSTS_INUSE_STS	0x40
#define SMBHSTSTS_SMBALERT_STS	0x20
#define SMBHSTSTS_FAILED	0x10
#define SMBHSTSTS_BUS_ERR	0x08
#define SMBHSTSTS_DEV_ERR	0x04
#define SMBHSTSTS_INTR		0x02
#define SMBHSTSTS_HOST_BUSY	0x01

#define STATUS_FLAGS		(SMBHSTSTS_BYTE_DONE | SMBHSTSTS_FAILED | \
				 SMBHSTSTS_BUS_ERR | SMBHSTSTS_DEV_ERR | \
				 SMBHSTSTS_INTR)

/* Older devices have their ID defined in <linux/pci_ids.h> */
#define PCI_DEVICE_ID_INTEL_COUGARPOINT_SMBUS	0x1c22
#define PCI_DEVICE_ID_INTEL_PATSBURG_SMBUS	0x1d22
/* Patsburg also has three 'Integrated Device Function' SMBus controllers */
#define PCI_DEVICE_ID_INTEL_PATSBURG_SMBUS_IDF0	0x1d70
#define PCI_DEVICE_ID_INTEL_PATSBURG_SMBUS_IDF1	0x1d71
#define PCI_DEVICE_ID_INTEL_PATSBURG_SMBUS_IDF2	0x1d72
#define PCI_DEVICE_ID_INTEL_PANTHERPOINT_SMBUS	0x1e22
#define PCI_DEVICE_ID_INTEL_AVOTON_SMBUS	0x1f3c
#define PCI_DEVICE_ID_INTEL_DH89XXCC_SMBUS	0x2330
#define PCI_DEVICE_ID_INTEL_5_3400_SERIES_SMBUS	0x3b30
#define PCI_DEVICE_ID_INTEL_LYNXPOINT_SMBUS	0x8c22
#define PCI_DEVICE_ID_INTEL_LYNXPOINT_LP_SMBUS	0x9c22

struct i801_priv {
	struct i2c_adapter adapter;
	unsigned long smba;
	unsigned char original_hstcfg;
	struct pci_dev *pci_dev;
	unsigned int features;
};

static struct pci_driver i801_driver;

#define FEATURE_SMBUS_PEC	(1 << 0)
#define FEATURE_BLOCK_BUFFER	(1 << 1)
#define FEATURE_BLOCK_PROC	(1 << 2)
#define FEATURE_I2C_BLOCK_READ	(1 << 3)
/* Not really a feature, but it's convenient to handle it as such */
#define FEATURE_IDF		(1 << 15)

static const char *i801_feature_names[] = {
	"SMBus PEC",
	"Block buffer",
	"Block process call",
	"I2C block read",
};

static unsigned int disable_features;
module_param(disable_features, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(disable_features, "Disable selected driver features");

/* Make sure the SMBus host is ready to start transmitting.
   Return 0 if it is, -EBUSY if it is not. */
static int i801_check_pre(struct i801_priv *priv)
{
	int status;

	status = inb_p(SMBHSTSTS(priv));
	if (status & SMBHSTSTS_HOST_BUSY) {
		dev_err(&priv->pci_dev->dev, "SMBus is busy, can't use it!\n");
		return -EBUSY;
	}

	status &= STATUS_FLAGS;
	if (status) {
		dev_dbg(&priv->pci_dev->dev, "Clearing status flags (%02x)\n",
			status);
		outb_p(status, SMBHSTSTS(priv));
		status = inb_p(SMBHSTSTS(priv)) & STATUS_FLAGS;
		if (status) {
			dev_err(&priv->pci_dev->dev,
				"Failed clearing status flags (%02x)\n",
				status);
			return -EBUSY;
		}
	}

	return 0;
}

/* Convert the status register to an error code, and clear it. */
static int i801_check_post(struct i801_priv *priv, int status, int timeout)
{
	int result = 0;

	/* If the SMBus is still busy, we give up */
	if (timeout) {
		dev_err(&priv->pci_dev->dev, "Transaction timeout\n");
		/* try to stop the current command */
		dev_dbg(&priv->pci_dev->dev, "Terminating the current operation\n");
		outb_p(SMBHSTCNT_KILL, SMBHSTCNT(priv));
		usleep_range(1000, 2000);
		outb_p(0, SMBHSTCNT(priv));

		/* Check if it worked */
		status = inb_p(SMBHSTSTS(priv));
		if ((status & SMBHSTSTS_HOST_BUSY) ||
		    !(status & SMBHSTSTS_FAILED))
			dev_err(&priv->pci_dev->dev,
				"Failed terminating the transaction\n");
		outb_p(STATUS_FLAGS, SMBHSTSTS(priv));
		return -ETIMEDOUT;
	}

	if (status & SMBHSTSTS_FAILED) {
		result = -EIO;
		dev_err(&priv->pci_dev->dev, "Transaction failed\n");
	}
	if (status & SMBHSTSTS_DEV_ERR) {
		result = -ENXIO;
		dev_dbg(&priv->pci_dev->dev, "No response\n");
	}
	if (status & SMBHSTSTS_BUS_ERR) {
		result = -EAGAIN;
		dev_dbg(&priv->pci_dev->dev, "Lost arbitration\n");
	}

	if (result) {
		/* Clear error flags */
		outb_p(status & STATUS_FLAGS, SMBHSTSTS(priv));
		status = inb_p(SMBHSTSTS(priv)) & STATUS_FLAGS;
		if (status) {
			dev_warn(&priv->pci_dev->dev, "Failed clearing status "
				 "flags at end of transaction (%02x)\n",
				 status);
		}
	}

	return result;
}

static int i801_transaction(struct i801_priv *priv, int xact)
{
	int status;
	int result;
	int timeout = 0;

	result = i801_check_pre(priv);
	if (result < 0)
		return result;

	/* the current contents of SMBHSTCNT can be overwritten, since PEC,
	 * INTREN, SMBSCMD are passed in xact */
	outb_p(xact | I801_START, SMBHSTCNT(priv));

	/* We will always wait for a fraction of a second! */
	do {
		usleep_range(250, 500);
		status = inb_p(SMBHSTSTS(priv));
	} while ((status & SMBHSTSTS_HOST_BUSY) && (timeout++ < MAX_RETRIES));

	result = i801_check_post(priv, status, timeout > MAX_RETRIES);
	if (result < 0)
		return result;

	outb_p(SMBHSTSTS_INTR, SMBHSTSTS(priv));
	return 0;
}

/* wait for INTR bit as advised by Intel */
static void i801_wait_hwpec(struct i801_priv *priv)
{
	int timeout = 0;
	int status;

	do {
		usleep_range(250, 500);
		status = inb_p(SMBHSTSTS(priv));
	} while ((!(status & SMBHSTSTS_INTR))
		 && (timeout++ < MAX_RETRIES));

	if (timeout > MAX_RETRIES)
		dev_dbg(&priv->pci_dev->dev, "PEC Timeout!\n");

	outb_p(status, SMBHSTSTS(priv));
}

static int i801_block_transaction_by_block(struct i801_priv *priv,
					   union i2c_smbus_data *data,
					   char read_write, int hwpec)
{
	int i, len;
	int status;

	inb_p(SMBHSTCNT(priv)); /* reset the data buffer index */

	/* Use 32-byte buffer to process this transaction */
	if (read_write == I2C_SMBUS_WRITE) {
		len = data->block[0];
		outb_p(len, SMBHSTDAT0(priv));
		for (i = 0; i < len; i++)
			outb_p(data->block[i+1], SMBBLKDAT(priv));
	}

	status = i801_transaction(priv, I801_BLOCK_DATA | ENABLE_INT9 |
				  I801_PEC_EN * hwpec);
	if (status)
		return status;

	if (read_write == I2C_SMBUS_READ) {
		len = inb_p(SMBHSTDAT0(priv));
		if (len < 1 || len > I2C_SMBUS_BLOCK_MAX)
			return -EPROTO;

		data->block[0] = len;
		for (i = 0; i < len; i++)
			data->block[i + 1] = inb_p(SMBBLKDAT(priv));
	}
	return 0;
}

static int i801_block_transaction_byte_by_byte(struct i801_priv *priv,
					       union i2c_smbus_data *data,
					       char read_write, int command,
					       int hwpec)
{
	int i, len;
	int smbcmd;
	int status;
	int result;
	int timeout;

	result = i801_check_pre(priv);
	if (result < 0)
		return result;

	len = data->block[0];

	if (read_write == I2C_SMBUS_WRITE) {
		outb_p(len, SMBHSTDAT0(priv));
		outb_p(data->block[1], SMBBLKDAT(priv));
	}

	for (i = 1; i <= len; i++) {
		if (i == len && read_write == I2C_SMBUS_READ) {
			if (command == I2C_SMBUS_I2C_BLOCK_DATA)
				smbcmd = I801_I2C_BLOCK_LAST;
			else
				smbcmd = I801_BLOCK_LAST;
		} else {
			if (command == I2C_SMBUS_I2C_BLOCK_DATA
			 && read_write == I2C_SMBUS_READ)
				smbcmd = I801_I2C_BLOCK_DATA;
			else
				smbcmd = I801_BLOCK_DATA;
		}
		outb_p(smbcmd | ENABLE_INT9, SMBHSTCNT(priv));

		if (i == 1)
			outb_p(inb(SMBHSTCNT(priv)) | I801_START,
			       SMBHSTCNT(priv));

		/* We will always wait for a fraction of a second! */
		timeout = 0;
		do {
			usleep_range(250, 500);
			status = inb_p(SMBHSTSTS(priv));
		} while ((!(status & SMBHSTSTS_BYTE_DONE))
			 && (timeout++ < MAX_RETRIES));

		result = i801_check_post(priv, status, timeout > MAX_RETRIES);
		if (result < 0)
			return result;

		if (i == 1 && read_write == I2C_SMBUS_READ
		 && command != I2C_SMBUS_I2C_BLOCK_DATA) {
			len = inb_p(SMBHSTDAT0(priv));
			if (len < 1 || len > I2C_SMBUS_BLOCK_MAX) {
				dev_err(&priv->pci_dev->dev,
					"Illegal SMBus block read size %d\n",
					len);
				/* Recover */
				while (inb_p(SMBHSTSTS(priv)) &
				       SMBHSTSTS_HOST_BUSY)
					outb_p(SMBHSTSTS_BYTE_DONE,
					       SMBHSTSTS(priv));
				outb_p(SMBHSTSTS_INTR, SMBHSTSTS(priv));
				return -EPROTO;
			}
			data->block[0] = len;
		}

		/* Retrieve/store value in SMBBLKDAT */
		if (read_write == I2C_SMBUS_READ)
			data->block[i] = inb_p(SMBBLKDAT(priv));
		if (read_write == I2C_SMBUS_WRITE && i+1 <= len)
			outb_p(data->block[i+1], SMBBLKDAT(priv));

		/* signals SMBBLKDAT ready */
		outb_p(SMBHSTSTS_BYTE_DONE | SMBHSTSTS_INTR, SMBHSTSTS(priv));
	}

	return 0;
}

static int i801_set_block_buffer_mode(struct i801_priv *priv)
{
	outb_p(inb_p(SMBAUXCTL(priv)) | SMBAUXCTL_E32B, SMBAUXCTL(priv));
	if ((inb_p(SMBAUXCTL(priv)) & SMBAUXCTL_E32B) == 0)
		return -EIO;
	return 0;
}

/* Block transaction function */
static int i801_block_transaction(struct i801_priv *priv,
				  union i2c_smbus_data *data, char read_write,
				  int command, int hwpec)
{
	int result = 0;
	unsigned char hostc;

	if (command == I2C_SMBUS_I2C_BLOCK_DATA) {
		if (read_write == I2C_SMBUS_WRITE) {
			/* set I2C_EN bit in configuration register */
			pci_read_config_byte(priv->pci_dev, SMBHSTCFG, &hostc);
			pci_write_config_byte(priv->pci_dev, SMBHSTCFG,
					      hostc | SMBHSTCFG_I2C_EN);
		} else if (!(priv->features & FEATURE_I2C_BLOCK_READ)) {
			dev_err(&priv->pci_dev->dev,
				"I2C block read is unsupported!\n");
			return -EOPNOTSUPP;
		}
	}

	if (read_write == I2C_SMBUS_WRITE
	 || command == I2C_SMBUS_I2C_BLOCK_DATA) {
		if (data->block[0] < 1)
			data->block[0] = 1;
		if (data->block[0] > I2C_SMBUS_BLOCK_MAX)
			data->block[0] = I2C_SMBUS_BLOCK_MAX;
	} else {
		data->block[0] = 32;	/* max for SMBus block reads */
	}

	/* Experience has shown that the block buffer can only be used for
	   SMBus (not I2C) block transactions, even though the datasheet
	   doesn't mention this limitation. */
	if ((priv->features & FEATURE_BLOCK_BUFFER)
	 && command != I2C_SMBUS_I2C_BLOCK_DATA
	 && i801_set_block_buffer_mode(priv) == 0)
		result = i801_block_transaction_by_block(priv, data,
							 read_write, hwpec);
	else
		result = i801_block_transaction_byte_by_byte(priv, data,
							     read_write,
							     command, hwpec);

	if (result == 0 && hwpec)
		i801_wait_hwpec(priv);

	if (command == I2C_SMBUS_I2C_BLOCK_DATA
	 && read_write == I2C_SMBUS_WRITE) {
		/* restore saved configuration register value */
		pci_write_config_byte(priv->pci_dev, SMBHSTCFG, hostc);
	}
	return result;
}

/* Return negative errno on error. */
static s32 i801_access(struct i2c_adapter *adap, u16 addr,
		       unsigned short flags, char read_write, u8 command,
		       int size, union i2c_smbus_data *data)
{
	int hwpec;
	int block = 0;
	int ret, xact = 0;
	struct i801_priv *priv = i2c_get_adapdata(adap);

	hwpec = (priv->features & FEATURE_SMBUS_PEC) && (flags & I2C_CLIENT_PEC)
		&& size != I2C_SMBUS_QUICK
		&& size != I2C_SMBUS_I2C_BLOCK_DATA;

	switch (size) {
	case I2C_SMBUS_QUICK:
		outb_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMBHSTADD(priv));
		xact = I801_QUICK;
		break;
	case I2C_SMBUS_BYTE:
		outb_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMBHSTADD(priv));
		if (read_write == I2C_SMBUS_WRITE)
			outb_p(command, SMBHSTCMD(priv));
		xact = I801_BYTE;
		break;
	case I2C_SMBUS_BYTE_DATA:
		outb_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMBHSTADD(priv));
		outb_p(command, SMBHSTCMD(priv));
		if (read_write == I2C_SMBUS_WRITE)
			outb_p(data->byte, SMBHSTDAT0(priv));
		xact = I801_BYTE_DATA;
		break;
	case I2C_SMBUS_WORD_DATA:
		outb_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMBHSTADD(priv));
		outb_p(command, SMBHSTCMD(priv));
		if (read_write == I2C_SMBUS_WRITE) {
			outb_p(data->word & 0xff, SMBHSTDAT0(priv));
			outb_p((data->word & 0xff00) >> 8, SMBHSTDAT1(priv));
		}
		xact = I801_WORD_DATA;
		break;
	case I2C_SMBUS_BLOCK_DATA:
		outb_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMBHSTADD(priv));
		outb_p(command, SMBHSTCMD(priv));
		block = 1;
		break;
	case I2C_SMBUS_I2C_BLOCK_DATA:
		/* NB: page 240 of ICH5 datasheet shows that the R/#W
		 * bit should be cleared here, even when reading */
		outb_p((addr & 0x7f) << 1, SMBHSTADD(priv));
		if (read_write == I2C_SMBUS_READ) {
			/* NB: page 240 of ICH5 datasheet also shows
			 * that DATA1 is the cmd field when reading */
			outb_p(command, SMBHSTDAT1(priv));
		} else
			outb_p(command, SMBHSTCMD(priv));
		block = 1;
		break;
	default:
		dev_err(&priv->pci_dev->dev, "Unsupported transaction %d\n",
			size);
		return -EOPNOTSUPP;
	}

	if (hwpec)	/* enable/disable hardware PEC */
		outb_p(inb_p(SMBAUXCTL(priv)) | SMBAUXCTL_CRC, SMBAUXCTL(priv));
	else
		outb_p(inb_p(SMBAUXCTL(priv)) & (~SMBAUXCTL_CRC),
		       SMBAUXCTL(priv));

	if (block)
		ret = i801_block_transaction(priv, data, read_write, size,
					     hwpec);
	else
		ret = i801_transaction(priv, xact | ENABLE_INT9);

	/* Some BIOSes don't like it when PEC is enabled at reboot or resume
	   time, so we forcibly disable it after every transaction. Turn off
	   E32B for the same reason. */
	if (hwpec || block)
		outb_p(inb_p(SMBAUXCTL(priv)) &
		       ~(SMBAUXCTL_CRC | SMBAUXCTL_E32B), SMBAUXCTL(priv));

	if (block)
		return ret;
	if (ret)
		return ret;
	if ((read_write == I2C_SMBUS_WRITE) || (xact == I801_QUICK))
		return 0;

	switch (xact & 0x7f) {
	case I801_BYTE:	/* Result put in SMBHSTDAT0 */
	case I801_BYTE_DATA:
		data->byte = inb_p(SMBHSTDAT0(priv));
		break;
	case I801_WORD_DATA:
		data->word = inb_p(SMBHSTDAT0(priv)) +
			     (inb_p(SMBHSTDAT1(priv)) << 8);
		break;
	}
	return 0;
}


static u32 i801_func(struct i2c_adapter *adapter)
{
	struct i801_priv *priv = i2c_get_adapdata(adapter);

	return I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |
	       I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
	       I2C_FUNC_SMBUS_BLOCK_DATA | I2C_FUNC_SMBUS_WRITE_I2C_BLOCK |
	       ((priv->features & FEATURE_SMBUS_PEC) ? I2C_FUNC_SMBUS_PEC : 0) |
	       ((priv->features & FEATURE_I2C_BLOCK_READ) ?
		I2C_FUNC_SMBUS_READ_I2C_BLOCK : 0);
}

static const struct i2c_algorithm smbus_algorithm = {
	.smbus_xfer	= i801_access,
	.functionality	= i801_func,
};

static DEFINE_PCI_DEVICE_TABLE(i801_ids) = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801AA_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801AB_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801BA_2) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801CA_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801DB_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801EB_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ESB_4) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH6_16) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH7_17) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ESB2_17) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH8_5) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH9_6) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_EP80579_1) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH10_4) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH10_5) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_5_3400_SERIES_SMBUS) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_COUGARPOINT_SMBUS) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_PATSBURG_SMBUS) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_PATSBURG_SMBUS_IDF0) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_PATSBURG_SMBUS_IDF1) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_PATSBURG_SMBUS_IDF2) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_DH89XXCC_SMBUS) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_PANTHERPOINT_SMBUS) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_LYNXPOINT_SMBUS) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_LYNXPOINT_LP_SMBUS) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_AVOTON_SMBUS) },
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, i801_ids);

#if defined CONFIG_X86 && defined CONFIG_DMI
static unsigned char apanel_addr;

/* Scan the system ROM for the signature "FJKEYINF" */
static __init const void __iomem *bios_signature(const void __iomem *bios)
{
	ssize_t offset;
	const unsigned char signature[] = "FJKEYINF";

	for (offset = 0; offset < 0x10000; offset += 0x10) {
		if (check_signature(bios + offset, signature,
				    sizeof(signature)-1))
			return bios + offset;
	}
	return NULL;
}

static void __init input_apanel_init(void)
{
	void __iomem *bios;
	const void __iomem *p;

	bios = ioremap(0xF0000, 0x10000); /* Can't fail */
	p = bios_signature(bios);
	if (p) {
		/* just use the first address */
		apanel_addr = readb(p + 8 + 3) >> 1;
	}
	iounmap(bios);
}

struct dmi_onboard_device_info {
	const char *name;
	u8 type;
	unsigned short i2c_addr;
	const char *i2c_type;
};

static struct dmi_onboard_device_info __devinitdata dmi_devices[] = {
	{ "Syleus", DMI_DEV_TYPE_OTHER, 0x73, "fscsyl" },
	{ "Hermes", DMI_DEV_TYPE_OTHER, 0x73, "fscher" },
	{ "Hades",  DMI_DEV_TYPE_OTHER, 0x73, "fschds" },
};

static void __devinit dmi_check_onboard_device(u8 type, const char *name,
					       struct i2c_adapter *adap)
{
	int i;
	struct i2c_board_info info;

	for (i = 0; i < ARRAY_SIZE(dmi_devices); i++) {
		/* & ~0x80, ignore enabled/disabled bit */
		if ((type & ~0x80) != dmi_devices[i].type)
			continue;
		if (strcasecmp(name, dmi_devices[i].name))
			continue;

		memset(&info, 0, sizeof(struct i2c_board_info));
		info.addr = dmi_devices[i].i2c_addr;
		strlcpy(info.type, dmi_devices[i].i2c_type, I2C_NAME_SIZE);
		i2c_new_device(adap, &info);
		break;
	}
}

/* We use our own function to check for onboard devices instead of
   dmi_find_device() as some buggy BIOS's have the devices we are interested
   in marked as disabled */
static void __devinit dmi_check_onboard_devices(const struct dmi_header *dm,
						void *adap)
{
	int i, count;

	if (dm->type != 10)
		return;

	count = (dm->length - sizeof(struct dmi_header)) / 2;
	for (i = 0; i < count; i++) {
		const u8 *d = (char *)(dm + 1) + (i * 2);
		const char *name = ((char *) dm) + dm->length;
		u8 type = d[0];
		u8 s = d[1];

		if (!s)
			continue;
		s--;
		while (s > 0 && name[0]) {
			name += strlen(name) + 1;
			s--;
		}
		if (name[0] == 0) /* Bogus string reference */
			continue;

		dmi_check_onboard_device(type, name, adap);
	}
}

/* Register optional slaves */
static void __devinit i801_probe_optional_slaves(struct i801_priv *priv)
{
	/* Only register slaves on main SMBus channel */
	if (priv->features & FEATURE_IDF)
		return;

	if (apanel_addr) {
		struct i2c_board_info info;

		memset(&info, 0, sizeof(struct i2c_board_info));
		info.addr = apanel_addr;
		strlcpy(info.type, "fujitsu_apanel", I2C_NAME_SIZE);
		i2c_new_device(&priv->adapter, &info);
	}

	if (dmi_name_in_vendors("FUJITSU"))
		dmi_walk(dmi_check_onboard_devices, &priv->adapter);
}
#else
static void __init input_apanel_init(void) {}
static void __devinit i801_probe_optional_slaves(struct i801_priv *priv) {}
#endif	/* CONFIG_X86 && CONFIG_DMI */

static int __devinit i801_probe(struct pci_dev *dev,
				const struct pci_device_id *id)
{
	unsigned char temp;
	int err, i;
	struct i801_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	i2c_set_adapdata(&priv->adapter, priv);
	priv->adapter.owner = THIS_MODULE;
	priv->adapter.class = I2C_CLASS_HWMON | I2C_CLASS_SPD;
	priv->adapter.algo = &smbus_algorithm;

	priv->pci_dev = dev;
	switch (dev->device) {
	case PCI_DEVICE_ID_INTEL_PATSBURG_SMBUS_IDF0:
	case PCI_DEVICE_ID_INTEL_PATSBURG_SMBUS_IDF1:
	case PCI_DEVICE_ID_INTEL_PATSBURG_SMBUS_IDF2:
		priv->features |= FEATURE_IDF;
		/* fall through */
	default:
		priv->features |= FEATURE_I2C_BLOCK_READ;
		/* fall through */
	case PCI_DEVICE_ID_INTEL_82801DB_3:
		priv->features |= FEATURE_SMBUS_PEC;
		priv->features |= FEATURE_BLOCK_BUFFER;
		/* fall through */
	case PCI_DEVICE_ID_INTEL_82801CA_3:
	case PCI_DEVICE_ID_INTEL_82801BA_2:
	case PCI_DEVICE_ID_INTEL_82801AB_3:
	case PCI_DEVICE_ID_INTEL_82801AA_3:
		break;
	}

	/* Disable features on user request */
	for (i = 0; i < ARRAY_SIZE(i801_feature_names); i++) {
		if (priv->features & disable_features & (1 << i))
			dev_notice(&dev->dev, "%s disabled by user\n",
				   i801_feature_names[i]);
	}
	priv->features &= ~disable_features;

	err = pci_enable_device(dev);
	if (err) {
		dev_err(&dev->dev, "Failed to enable SMBus PCI device (%d)\n",
			err);
		goto exit;
	}

	/* Determine the address of the SMBus area */
	priv->smba = pci_resource_start(dev, SMBBAR);
	if (!priv->smba) {
		dev_err(&dev->dev, "SMBus base address uninitialized, "
			"upgrade BIOS\n");
		err = -ENODEV;
		goto exit;
	}

	err = acpi_check_resource_conflict(&dev->resource[SMBBAR]);
	if (err) {
		err = -ENODEV;
		goto exit;
	}

	err = pci_request_region(dev, SMBBAR, i801_driver.name);
	if (err) {
		dev_err(&dev->dev, "Failed to request SMBus region "
			"0x%lx-0x%Lx\n", priv->smba,
			(unsigned long long)pci_resource_end(dev, SMBBAR));
		goto exit;
	}

	pci_read_config_byte(priv->pci_dev, SMBHSTCFG, &temp);
	priv->original_hstcfg = temp;
	temp &= ~SMBHSTCFG_I2C_EN;	/* SMBus timing */
	if (!(temp & SMBHSTCFG_HST_EN)) {
		dev_info(&dev->dev, "Enabling SMBus device\n");
		temp |= SMBHSTCFG_HST_EN;
	}
	pci_write_config_byte(priv->pci_dev, SMBHSTCFG, temp);

	if (temp & SMBHSTCFG_SMB_SMI_EN)
		dev_dbg(&dev->dev, "SMBus using interrupt SMI#\n");
	else
		dev_dbg(&dev->dev, "SMBus using PCI Interrupt\n");

	/* Clear special mode bits */
	if (priv->features & (FEATURE_SMBUS_PEC | FEATURE_BLOCK_BUFFER))
		outb_p(inb_p(SMBAUXCTL(priv)) &
		       ~(SMBAUXCTL_CRC | SMBAUXCTL_E32B), SMBAUXCTL(priv));

	/* set up the sysfs linkage to our parent device */
	priv->adapter.dev.parent = &dev->dev;

	/* Retry up to 3 times on lost arbitration */
	priv->adapter.retries = 3;

	snprintf(priv->adapter.name, sizeof(priv->adapter.name),
		"SMBus I801 adapter at %04lx", priv->smba);
	err = i2c_add_adapter(&priv->adapter);
	if (err) {
		dev_err(&dev->dev, "Failed to add SMBus adapter\n");
		goto exit_release;
	}

	i801_probe_optional_slaves(priv);

	pci_set_drvdata(dev, priv);
	return 0;

exit_release:
	pci_release_region(dev, SMBBAR);
exit:
	kfree(priv);
	return err;
}

static void __devexit i801_remove(struct pci_dev *dev)
{
	struct i801_priv *priv = pci_get_drvdata(dev);

	i2c_del_adapter(&priv->adapter);
	pci_write_config_byte(dev, SMBHSTCFG, priv->original_hstcfg);
	pci_release_region(dev, SMBBAR);
	pci_set_drvdata(dev, NULL);
	kfree(priv);
	/*
	 * do not call pci_disable_device(dev) since it can cause hard hangs on
	 * some systems during power-off (eg. Fujitsu-Siemens Lifebook E8010)
	 */
}

#ifdef CONFIG_PM
static int i801_suspend(struct pci_dev *dev, pm_message_t mesg)
{
	struct i801_priv *priv = pci_get_drvdata(dev);

	pci_save_state(dev);
	pci_write_config_byte(dev, SMBHSTCFG, priv->original_hstcfg);
	pci_set_power_state(dev, pci_choose_state(dev, mesg));
	return 0;
}

static int i801_resume(struct pci_dev *dev)
{
	pci_set_power_state(dev, PCI_D0);
	pci_restore_state(dev);
	return pci_enable_device(dev);
}
#else
#define i801_suspend NULL
#define i801_resume NULL
#endif

static struct pci_driver i801_driver = {
	.name		= "i801_smbus",
	.id_table	= i801_ids,
	.probe		= i801_probe,
	.remove		= __devexit_p(i801_remove),
	.suspend	= i801_suspend,
	.resume		= i801_resume,
};

static int __init i2c_i801_init(void)
{
	if (dmi_name_in_vendors("FUJITSU"))
		input_apanel_init();
	return pci_register_driver(&i801_driver);
}

static void __exit i2c_i801_exit(void)
{
	pci_unregister_driver(&i801_driver);
}

MODULE_AUTHOR("Mark D. Studebaker <mdsxyz123@yahoo.com>, "
	      "Jean Delvare <khali@linux-fr.org>");
MODULE_DESCRIPTION("I801 SMBus driver");
MODULE_LICENSE("GPL");

module_init(i2c_i801_init);
module_exit(i2c_i801_exit);
