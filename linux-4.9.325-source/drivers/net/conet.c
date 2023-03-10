
/*
 *  Copyright (C) 2003-2004 Dan Aloni <da-x@gmx.net>
 *  Copyright (C) 2004 Pat Erley
 *  Copyright (C) 2004 George Boutwell
 *  Copyright (C) 2007 Steve Shoecraft <sshoecraft@earthlink.net>
 *
 *  Cooperative Linux Network Device implementation
 */

#include <linux/version.h>
#include <linux/module.h>

#include <linux/kernel.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/ethtool.h>
#include <linux/mii.h>
#include <linux/pci.h>

#include <linux/cooperative_internal.h>
#include <linux/cooperative_pci.h>
#include <asm/irq.h>

#define DRV_NAME 	"conet"
#define DRV_VERSION	"1.02"

struct conet_priv {
	struct net_device_stats stats;
	int unit;
	unsigned short flags;
	struct pci_dev *pdev;
	spinlock_t rx_lock;
	spinlock_t ioctl_lock;
	struct mii_if_info mii_if;
};

#define CONET_FLAG_ENABLED	0x01
#define CONET_FLAG_HANDLING	0x02
#define CONET_FLAG_DEBUG	0x80

static struct net_device *conet_dev[CO_MODULE_MAX_CONET];

static int conet_open(struct net_device *dev)
{
	struct conet_priv *priv = netdev_priv(dev);

	if (priv->flags & CONET_FLAG_ENABLED) return 0;

	priv->flags |= CONET_FLAG_ENABLED;

	netif_start_queue(dev);

	return 0;
}

static int conet_stop(struct net_device *dev)
{
	struct conet_priv *priv = netdev_priv(dev);

	priv->flags &= ~CONET_FLAG_ENABLED;

	netif_stop_queue(dev);

	return 0;
}

static int conet_hard_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	int len;
	char *data;
	struct conet_priv *priv = netdev_priv(dev);

	len = skb->len < ETH_ZLEN ? ETH_ZLEN : skb->len;
	data = skb->data;

	dev->trans_start = jiffies; /* save the timestamp */

	co_send_message(CO_MODULE_LINUX,
			CO_MODULE_CONET0 + priv->unit,
			CO_PRIORITY_DISCARDABLE,
			CO_MESSAGE_TYPE_OTHER,
			len,
			data);

	priv->stats.tx_bytes+=skb->len;
	priv->stats.tx_packets++;

	dev_kfree_skb(skb);

	return 0;
}

static void conet_rx(struct net_device *dev, co_linux_message_t *message)
{
	struct sk_buff *skb;
	struct conet_priv *priv = netdev_priv(dev);
	int len;
	unsigned char *buf;

	len = message->size;
	if (len > 0x10000) {
		printk("conet rx: buggy network reception\n");
		priv->stats.rx_dropped++;
		return;
	}

	buf = message->data;

	/*
	 * The packet has been retrieved from the transmission
	 * medium. Build an skb around it, so upper layers can handle it
	 */
	skb = dev_alloc_skb(len+2);
	if (!skb) {
		printk("conet rx: low on mem - packet dropped\n");
		priv->stats.rx_dropped++;
		return;
	}

	memcpy(skb_put(skb, len), buf, len);

	/* Write metadata, and then pass to the receive level */
	skb->dev = dev;
	skb->protocol = eth_type_trans(skb, dev);
	skb->ip_summed = CHECKSUM_NONE; /* make the kernel calculate and verify
                                           the checksum */

	priv->stats.rx_bytes += len;
	priv->stats.rx_packets++;

	netif_rx(skb);
	return;
}

static irqreturn_t conet_interrupt(int irq, void *dev_id)
{
	co_message_node_t *node_message;

	while (co_get_message(&node_message, CO_DEVICE_NETWORK)) {
		struct net_device *dev;
		struct conet_priv *priv;
		co_linux_message_t *message;

		message = (co_linux_message_t *)&node_message->msg.data;
		if (message->unit >= CO_MODULE_MAX_CONET) {
			printk("conet interrupt: buggy network reception unit %d\n", message->unit);
			return IRQ_HANDLED;
		}

		dev = conet_dev[message->unit];
		if (!dev) {
			co_free_message(node_message);
			continue;
		}

		if (!netif_running(dev)) {
			co_free_message(node_message);
			continue;
		}

		if (node_message->msg.type == CO_MESSAGE_TYPE_STRING) {
			int connected= *(int*)(message+1);
			if (connected)
				netif_carrier_on(dev);
			else
				netif_carrier_off(dev);
			co_free_message(node_message);
			continue;
		}

		priv = netdev_priv(dev);
		spin_lock(&priv->rx_lock);
#if 0
		if (priv->flags & CONET_FLAG_HANDLING) {
			co_free_message(node_message);
			continue;
		}

		priv->flags |= CONET_FLAG_HANDLING;
		conet_rx(dev, message);
		co_free_message(node_message);
		priv->flags &= ~CONET_FLAG_HANDLING;
#endif
		conet_rx(dev, message);
		co_free_message(node_message);
		spin_unlock(&priv->rx_lock);
	}

	return IRQ_HANDLED;
}

static struct net_device_stats* conet_get_stats(struct net_device *dev)
{
	struct conet_priv *priv = netdev_priv(dev);

	return &priv->stats;
}

static int conet_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	/* We support 100-baseT Full duplex TP */
	cmd->port = PORT_TP;
	cmd->duplex = DUPLEX_FULL;
	cmd->supported = SUPPORTED_TP | SUPPORTED_100baseT_Full;
	cmd->speed = SPEED_100;
	return 0;
}

static int conet_set_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	/* We support TP Full duplex 100 */
	if (cmd->port != PORT_TP || cmd->duplex != DUPLEX_FULL || cmd->speed != SPEED_100)
		return -EOPNOTSUPP;
	return 0;
}

static void conet_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	struct conet_priv *priv = netdev_priv(dev);

	strcpy(info->driver, DRV_NAME);
	strcpy(info->version, DRV_VERSION);
	strcpy(info->bus_info, pci_name(priv->pdev));
}

static u32 conet_get_link(struct net_device *dev)
{
	/* Always connected */
	return 1;
}

static u32 conet_get_msglevel(struct net_device *dev)
{
	struct conet_priv *priv = netdev_priv(dev);

        return ((priv->flags & CONET_FLAG_DEBUG) != 0);
}

static void conet_set_msglevel(struct net_device *dev, u32 level)
{
	struct conet_priv *priv = netdev_priv(dev);

	if (level)
		priv->flags |= CONET_FLAG_DEBUG;
	else
		priv->flags &= ~CONET_FLAG_DEBUG;
}

static int conet_mdio_read(struct net_device *dev, int id, int reg)
{
	struct conet_priv *priv = netdev_priv(dev);
	int val;

	if (priv->flags & CONET_FLAG_DEBUG)
		printk(KERN_INFO "conet%d: mdio_read: id: %d, reg: %d\n", priv->unit, id, reg);
	switch(reg) {
	case MII_BMCR:			/* Basic mode control register */
		val = BMCR_FULLDPLX | BMCR_SPEED100;
		break;
	case MII_BMSR:			/* Basic mode status register  */
		val = BMSR_LSTATUS | BMSR_100FULL;
		break;
	default:
		val = 0;
		break;
	}
	return val;
}

static void conet_mdio_write(struct net_device *dev, int id, int reg, int val)
{
	struct conet_priv *priv = netdev_priv(dev);

	if (priv->flags & CONET_FLAG_DEBUG)
		printk(KERN_INFO "conet%d: mdio_write: id: %d, reg: %d, val: %d\n", priv->unit, id, reg, val);
	return;
}

static int conet_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct conet_priv *priv = netdev_priv(dev);
	int rc;

	spin_lock(&priv->ioctl_lock);
	rc = generic_mii_ioctl(&priv->mii_if, if_mii(rq), cmd, NULL);
	spin_unlock(&priv->ioctl_lock);

	return rc;
}

static const struct ethtool_ops conet_ethtool_ops = {
	.get_settings           = conet_get_settings,
	.set_settings           = conet_set_settings,
	.get_drvinfo            = conet_get_drvinfo,
	.get_link               = conet_get_link,
	.get_msglevel           = conet_get_msglevel,
	.set_msglevel           = conet_set_msglevel,
#if 0
	.nway_reset             = conet_nway_reset,
	.get_ringparam          = conet_get_ringparam,
	.set_ringparam          = conet_set_ringparam,
	.get_tx_csum            = ethtool_op_get_tx_csum,
	.get_sg                 = ethtool_op_get_sg,
	.get_tso                = ethtool_op_get_tso,
	.get_strings            = conet_get_strings,
	.self_test_count        = conet_self_test_count,
	.self_test              = conet_ethtool_test,
	.phys_id                = conet_phys_id,
	.get_regs_len           = conet_get_regs_len,
	.get_regs               = conet_get_regs,
	.get_perm_addr          = ethtool_op_get_perm_addr,
#endif
};

static struct pci_device_id conet_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CO, PCI_DEVICE_ID_CONET) },
	{ 0 }
};

MODULE_DEVICE_TABLE(pci, conet_pci_ids);

static const struct net_device_ops net_ops = {
	.ndo_open		= conet_open,
	.ndo_stop		= conet_stop,
	.ndo_start_xmit 	= conet_hard_start_xmit,
	.ndo_get_stats		= conet_get_stats,
	.ndo_do_ioctl		= conet_ioctl,
};

static int __devinit conet_pci_probe( struct pci_dev *pdev,
                                    const struct pci_device_id *ent)
{
	u8 unit, addr[6];
	struct net_device *dev;
	struct conet_priv *priv;
	int rc;

//	printk(KERN_INFO "CONET: probing!\n");

	unit = -1;
	pci_read_config_byte(pdev, PCI_CO_UNIT, &unit);
	pci_read_config_byte(pdev, PCI_CO_MAC1, &addr[0]);
	pci_read_config_byte(pdev, PCI_CO_MAC2, &addr[1]);
	pci_read_config_byte(pdev, PCI_CO_MAC3, &addr[2]);
	pci_read_config_byte(pdev, PCI_CO_MAC4, &addr[3]);
	pci_read_config_byte(pdev, PCI_CO_MAC5, &addr[4]);
	pci_read_config_byte(pdev, PCI_CO_MAC6, &addr[5]);

	dev = alloc_etherdev(sizeof(*priv));
	if (dev == NULL) {
		printk(KERN_ERR "conet%d: could not allocate memory for device.\n", unit);
		rc = -ENOMEM;
		goto error_out_pdev;
	}
	SET_NETDEV_DEV(dev, &pdev->dev);
	memcpy(dev->dev_addr, addr, 6);

	dev->netdev_ops	= &net_ops;
	dev->ethtool_ops = &conet_ethtool_ops;
	dev->irq = pdev->irq;

	priv = netdev_priv(dev);
	priv->unit = unit;
	priv->pdev = pdev;

	spin_lock_init(&priv->ioctl_lock);
	spin_lock_init(&priv->rx_lock);

	priv->mii_if.full_duplex = 1;
	priv->mii_if.phy_id_mask = 0x1f;
	priv->mii_if.reg_num_mask = 0x1f;
	priv->mii_if.dev = dev;
	priv->mii_if.mdio_read = conet_mdio_read;
	priv->mii_if.mdio_write = conet_mdio_write;
	priv->mii_if.phy_id = 1;

	pci_set_drvdata(pdev, priv);

	rc = register_netdev(dev);
	if (rc) {
		printk(KERN_ERR "conet%d: could not register device; rc: %d\n", unit, rc);
		goto error_out_dev;
	}

	conet_dev[unit] = dev;

	printk(KERN_INFO "conet%d: irq %d, HWAddr %02x:%02x:%02x:%02x:%02x:%02x\n",
		unit, NETWORK_IRQ, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

	return 0;

error_out_dev:
	free_netdev(dev);

error_out_pdev:
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);

	return rc;
}

static void __devexit conet_pci_remove(struct pci_dev *pdev)
{
	struct conet_priv *priv = dev_get_drvdata(&pdev->dev);
	struct net_device *net_dev = conet_dev[priv->unit];

	unregister_netdev(net_dev);
	free_netdev(net_dev);
	dev_set_drvdata(&pdev->dev, NULL);
}

static struct pci_driver conet_pci_driver = {
	.name           = DRV_NAME,
	.id_table       = conet_pci_ids,
	.probe          = conet_pci_probe,
	.remove         = __devexit_p(conet_pci_remove),
};

static int __init conet_pci_init(void)
{
	int unit, rc;

//	printk(KERN_INFO "CONET: Initializing...\n");

	rc = request_irq(NETWORK_IRQ, &conet_interrupt, IRQF_SAMPLE_RANDOM, "conet", NULL);
	if (rc) {
		printk(KERN_ERR "CONET: unable to get irq %d", NETWORK_IRQ);
		return rc;
	}

	/* Init our units */
	for (unit=0; unit < CO_MODULE_MAX_CONET; unit++)
		conet_dev[unit] = NULL;

//	printk(KERN_INFO "CONET: registering...\n");
        return pci_register_driver(&conet_pci_driver);
}

static void __exit conet_pci_exit(void)
{
        pci_unregister_driver(&conet_pci_driver);
}

module_init(conet_pci_init);
module_exit(conet_pci_exit);
