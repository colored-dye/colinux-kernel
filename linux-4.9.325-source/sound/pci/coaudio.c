
#include <sound/driver.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/slab.h>

#include <sound/core.h>
#include <sound/control.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include <linux/cooperative.h>
#include <linux/cooperative_internal.h>
#include <linux/cooperative_pci.h>

extern int pci_debug;

MODULE_AUTHOR("Steve Shoecraft <sshoecraft@earthlink.net>");
MODULE_DESCRIPTION("Cooperative Linux Audio Driver");
MODULE_LICENSE("GPL");

#define COAUDIO_DEBUG 0

typedef struct {
	struct pci_dev *pdev;
	struct snd_card *card;
	int irq;
} coaudio_dev_t;

static irqreturn_t coaudio_isr(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}

static int coaudio_playback_open(struct snd_pcm_substream *substream)
{
        return -EIO;
}

static int coaudio_playback_close(struct snd_pcm_substream *substream)
{
	return -EIO;
}

static int coaudio_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params)
{
	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}

static int coaudio_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static int coaudio_playback_prepare(struct snd_pcm_substream *substream)
{
	return -EIO;
}

static int coaudio_trigger(struct snd_pcm_substream *substream, int cmd)
{
	return -EIO;
}

static snd_pcm_uframes_t coaudio_pointer(struct snd_pcm_substream *substream)
{
	u16 current_ptr = 0;

	return bytes_to_frames(substream->runtime, current_ptr);
}

static struct snd_pcm_ops coaudio_playback_ops = {
	.open =		coaudio_playback_open,
	.close =	coaudio_playback_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	coaudio_pcm_hw_params,
	.hw_free =	coaudio_pcm_hw_free,
	.prepare =	coaudio_playback_prepare,
	.trigger =	coaudio_trigger,
	.pointer =	coaudio_pointer,
};

static int __devinit coaudio_new_pcm(coaudio_dev_t *dev)
{
	struct snd_pcm *pcm;
	int err;

	err = snd_pcm_new(dev->card, "COAUDIO", 0, 1, 1, &pcm);
	if (err < 0) return err;
	strcpy(pcm->name, "coaudio");

	/* set operators */
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &coaudio_playback_ops);

	/* pre-allocation of buffers */
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV, snd_dma_pci_data(dev->pdev), 64*1024, 64*1024);
	return 0;
}

static int coaudio_dev_free(struct snd_device *device)
{
	struct coaudio_dev_t *dev = device->device_data;

	kfree(dev);
	return 0;
}

static struct snd_device_ops coaudio_device_ops = {
	.dev_free = coaudio_dev_free,
};

/****************************************************************************************************
 *
 *
 * PCI functions
 *
 *
 ****************************************************************************************************/
static int __devinit coaudio_pci_probe( struct pci_dev *pdev, const struct pci_device_id *ent )
{
	struct snd_card *card;
	coaudio_dev_t *dev;
	int err, irq;

	irq = SOUND_IRQ;

	card = snd_card_new(SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1, THIS_MODULE, sizeof(coaudio_dev_t));
	if (!card) return -ENOMEM;

	strcpy(card->driver, "coaudio");
	sprintf(card->shortname, "coaudio");
	sprintf(card->longname, "%s: Cooperative Audio Device using irq %d", card->shortname, irq);
	if ((err = snd_card_register(card)) < 0) {
		snd_card_free(card);
		return err;
	}
	dev = card->private_data;
	dev->pdev = pdev;
	dev->card = card;
	dev->irq = irq;

	if ((err = coaudio_new_pcm(dev)) < 0) {
		printk(KERN_WARNING "COAUDIO: could not create PCM\n");
		snd_card_free(card);
		kfree(dev);
		return -EIO;
	}

	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, dev, &coaudio_device_ops)) < 0) {
		snd_card_free(card);
		kfree(dev);
		return -EIO;
	}

	snd_card_set_dev(card, &pdev->dev);

	if (request_irq(dev->irq, coaudio_isr, IRQF_SHARED, card->shortname, card)) {
		printk(KERN_ERR "coaudio: unable to allocate IRQ %d\n", dev->irq);
		snd_card_free(card);
		kfree(dev);
		return -EBUSY;
	}

	return 0;
}

static void __devexit coaudio_pci_remove(struct pci_dev *pdev)
{
	struct snd_card *card = pci_get_drvdata(pdev);
	coaudio_dev_t *dev = card->private_data;

	free_irq(dev->irq, card);
        snd_card_free(card);
        pci_set_drvdata(pdev, NULL);
}

static struct pci_device_id coaudio_pci_ids[] __devinitdata = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CO, PCI_DEVICE_ID_COAUDIO) },
	{ 0 }
};

static struct pci_driver coaudio_pci_driver = {
	.name = 	"coaudio",
	.id_table =	coaudio_pci_ids,
	.probe =	coaudio_pci_probe,
	.remove =	__devexit_p(coaudio_pci_remove),
#ifdef CONFIG_PM
	.suspend = 	coaudio_suspend,
	.resume =	coaudio_resume,
#endif
};

static int __init coaudio_pci_init(void)
{
	return pci_register_driver(&coaudio_pci_driver);
}

static void __exit coaudio_pci_exit(void)
{
	pci_unregister_driver(&coaudio_pci_driver);
}

module_init(coaudio_pci_init)
module_exit(coaudio_pci_exit)
