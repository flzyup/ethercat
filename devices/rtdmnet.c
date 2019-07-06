/******************************************************************************
 *
 *  $Id$
 *
 *  Copyright (C) 2006-2008  Florian Pose, Ingenieurgemeinschaft IgH
 *  Copyright (C) 2014-2018  Edouard Tisserant
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 *  The IgH EtherCAT Master is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 *  Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the IgH EtherCAT Master; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *  ---
 *
 *  The license mentioned above concerns the source code only. Using the
 *  EtherCAT technology and brand is only permitted in compliance with the
 *  industrial property and similar rights of Beckhoff Automation GmbH.
 *
 *****************************************************************************/

/** \file
 * EtherCAT generic Xenomai's RTDM RAW Ethernet socket device module.
 * Heavily based on generic.c. Should be merged in a single file with #ifdefs
 */

/*****************************************************************************/

#include <linux/module.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/version.h>
#include <linux/if_arp.h> /* ARPHRD_ETHER */
#include <linux/etherdevice.h>
#include <rtdm/rtdm.h>

// for rtnetif_carrier_ok and rtpc_dispatch_call
// This needs -I@XENOMAI_DIR@/kernel/drivers/net/stack/include in Kbuild.in
#include <rtnet_port.h>
#include <rtnet_rtpc.h>

#include "../globals.h"
#include "ecdev.h"

#define PFX "ec_rtdmnet: "

#define ETH_P_ETHERCAT 0x88A4

#define EC_GEN_RX_BUF_SIZE 1600

/*****************************************************************************/

int __init ec_gen_init_module(void);
void __exit ec_gen_cleanup_module(void);

/*****************************************************************************/

/** \cond */

MODULE_AUTHOR("Edouard Tisserant <edouard.tisserant@gmail.com>");
MODULE_DESCRIPTION("EtherCAT generic Xenomai's RTDM RAW Ethernet socket device module.");
MODULE_LICENSE("GPL");
MODULE_VERSION(EC_MASTER_VERSION);

/** \endcond */

struct list_head generic_devices;

typedef struct {
    struct list_head list;
    struct net_device *netdev;
    struct rtnet_device *used_netdev;
    int socket;
    struct rtdm_fd *rtdm_fd;
    ec_device_t *ecdev;
    uint8_t *rx_buf;
    // struct sockaddr_ll dest_addr;
} ec_gen_device_t;

typedef struct {
    struct list_head list;
    struct rtnet_device *netdev;
    char name[IFNAMSIZ];
    int ifindex;
    uint8_t dev_addr[ETH_ALEN];
} ec_gen_interface_desc_t;

int ec_gen_device_open(ec_gen_device_t *);
int ec_gen_device_stop(ec_gen_device_t *);
int ec_gen_device_start_xmit(ec_gen_device_t *, struct sk_buff *);
void ec_gen_device_poll(ec_gen_device_t *);

/*****************************************************************************/

static int ec_gen_netdev_open(struct net_device *dev)
{
    ec_gen_device_t *gendev = *((ec_gen_device_t **) netdev_priv(dev));
    return ec_gen_device_open(gendev);
}

/*****************************************************************************/

static int ec_gen_netdev_stop(struct net_device *dev)
{
    ec_gen_device_t *gendev = *((ec_gen_device_t **) netdev_priv(dev));
    return ec_gen_device_stop(gendev);
}

/*****************************************************************************/

static int ec_gen_netdev_start_xmit(
        struct sk_buff *skb,
        struct net_device *dev
        )
{
    ec_gen_device_t *gendev = *((ec_gen_device_t **) netdev_priv(dev));
    return ec_gen_device_start_xmit(gendev, skb);
}

/*****************************************************************************/

void ec_gen_poll(struct net_device *dev)
{
    ec_gen_device_t *gendev = *((ec_gen_device_t **) netdev_priv(dev));
    ec_gen_device_poll(gendev);
}

/*****************************************************************************/

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
static const struct net_device_ops ec_gen_netdev_ops = {
    .ndo_open       = ec_gen_netdev_open,
    .ndo_stop       = ec_gen_netdev_stop,
    .ndo_start_xmit = ec_gen_netdev_start_xmit,
};
#endif

/*****************************************************************************/

/** Init generic device.
 */
int ec_gen_device_init(
        ec_gen_device_t *dev
        )
{
    ec_gen_device_t **priv;
    char null = 0x00;

    dev->ecdev = NULL;
    dev->socket = -1;
    dev->rx_buf = NULL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 17, 0)
    dev->netdev = alloc_netdev(sizeof(ec_gen_device_t *), &null,
            NET_NAME_UNKNOWN, ether_setup);
#else
    dev->netdev = alloc_netdev(sizeof(ec_gen_device_t *), &null, ether_setup);
#endif
    if (!dev->netdev) {
        return -ENOMEM;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
    dev->netdev->netdev_ops = &ec_gen_netdev_ops;
#else
    dev->netdev->open = ec_gen_netdev_open;
    dev->netdev->stop = ec_gen_netdev_stop;
    dev->netdev->hard_start_xmit = ec_gen_netdev_start_xmit;
#endif

    priv = netdev_priv(dev->netdev);
    *priv = dev;

    return 0;
}

/*****************************************************************************/

/** Clear generic device.
 */
void ec_gen_device_clear(
        ec_gen_device_t *dev
        )
{
    if (dev->ecdev) {
        ecdev_close(dev->ecdev);
        ecdev_withdraw(dev->ecdev);
    }
    if (!(dev->socket < 0)) {
        rtdm_fd_put(dev->rtdm_fd);
        rtdm_close(dev->socket);
        dev->socket = -1;
    }
    free_netdev(dev->netdev);

    if (dev->rx_buf) {
        kfree(dev->rx_buf);
    }
}

/*****************************************************************************/

/** Creates a network socket.
 */
int ec_gen_device_create_socket(
        ec_gen_device_t *dev,
        ec_gen_interface_desc_t *desc
        )
{
    int ret;
    struct sockaddr_ll sa;

    dev->rx_buf = kmalloc(EC_GEN_RX_BUF_SIZE, GFP_KERNEL);
    if (!dev->rx_buf) {
        return -ENOMEM;
    }

    /* create rt-socket */
    dev->socket = rtdm_socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ETHERCAT));
    if (dev->socket < 0) {
        printk(" rtdm_socket() = %d!\n", dev->socket);
        return dev->socket;
    }
    
    /* HACK :
       When this is called, process is 'kernel', i.e. 
         cobalt_ppd_get(0) == &cobalt_kernel_ppd

       This makes a problem later when recvmsg or sendmsg is indirectly called
       from application's cobalt thread through RTDM IOCTL. From such thread, 
       RTDM can't resolv socket's 'ufd' that rtdm_socket just returned.

       To keep a usable file descriptor for that socket, even when calling 
       from cobalt thread (i.e. if (cobalt_ppd_get(0) != &cobalt_kernel_ppd))
       we resolve it here in advance */
    dev->rtdm_fd = rtdm_fd_get(dev->socket,0);
    if (IS_ERR(dev->rtdm_fd)){
        printk(" rtdm_fd_get() = %d!\n", ret);
        ret = PTR_ERR(dev->rtdm_fd);
        goto out_err_rtdm_fd;
    }

    printk(KERN_ERR PFX "Binding socket to interface %i (%s).\n",
            desc->ifindex, desc->name);

    memset(&sa, 0x00, sizeof(sa));
    sa.sll_family = AF_PACKET;
    sa.sll_protocol = htons(ETH_P_ETHERCAT);
    sa.sll_ifindex = desc->ifindex;
    ret = rtdm_bind(dev->socket, (struct sockaddr *)&sa,
                      sizeof(struct sockaddr_ll));
    if (ret < 0) {
        printk(" rtdm_bind() = %d!\n", ret);
        goto out_err_bind;
    }

    return 0;

out_err_bind:
    rtdm_fd_put(dev->rtdm_fd);
out_err_rtdm_fd:
    rtdm_close(dev->socket);
    dev->socket = -1;
    return ret;
    
}

/*****************************************************************************/

/** Offer generic device to master.
 */
int ec_gen_device_offer(
        ec_gen_device_t *dev,
        ec_gen_interface_desc_t *desc
        )
{
    int ret = 0;

    dev->used_netdev = desc->netdev;
    memcpy(dev->netdev->dev_addr, desc->dev_addr, ETH_ALEN);

    dev->ecdev = ecdev_offer(dev->netdev, ec_gen_poll, THIS_MODULE);
    if (dev->ecdev) {
        if (ec_gen_device_create_socket(dev, desc)) {
            ecdev_withdraw(dev->ecdev);
            dev->ecdev = NULL;
        } else if (ecdev_open(dev->ecdev)) {
            ecdev_withdraw(dev->ecdev);
            dev->ecdev = NULL;
        } else {
            ecdev_set_link(dev->ecdev, rtnetif_carrier_ok(dev->used_netdev)); // FIXME
            ret = 1;
        }
    }

    return ret;
}

/*****************************************************************************/

/** Open the device.
 */
int ec_gen_device_open(
        ec_gen_device_t *dev
        )
{
    return 0;
}

/*****************************************************************************/

/** Stop the device.
 */
int ec_gen_device_stop(
        ec_gen_device_t *dev
        )
{
    return 0;
}

/*****************************************************************************/

// delegate to some rtdm thread when called from nrt context
struct sendmsg_params {
    int socket;
    struct user_msghdr *msg;
};

static int sendmsg_handler(struct rt_proc_call *call)
{
    struct sendmsg_params *params;

    params = rtpc_get_priv(call, struct sendmsg_params);

    return rtdm_sendmsg(params->socket, params->msg, 0);
}

static ssize_t
nrt_rtdm_sendmsg(int socket, struct user_msghdr *msg)
{
    int ret;
    struct sendmsg_params params = {socket, msg};

    ret = rtpc_dispatch_call(sendmsg_handler, 0, &params,
                             sizeof(params), NULL, NULL);
    return ret;
}

int ec_gen_device_start_xmit(
        ec_gen_device_t *dev,
        struct sk_buff *skb
        )
{
    struct user_msghdr msg;
    struct iovec iov;
    size_t len = skb->len;
    int ret;

    ecdev_set_link(dev->ecdev, rtnetif_carrier_ok(dev->used_netdev));
    //ecdev_set_link(dev->ecdev, 1); // FIXME

    iov.iov_base = skb->data;
    iov.iov_len = len;
    memset(&msg, 0, sizeof(msg));
    // msg.msg_name    = &dev->dest_addr;
    // msg.msg_namelen = sizeof(dev->dest_addr);
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;

    if (rtdm_in_rt_context())
        /* HACK : call fd ops directly as in rtdm's fd.c */
        ret = dev->rtdm_fd->ops->sendmsg_rt(dev->rtdm_fd, &msg, 0);
    else
        ret = nrt_rtdm_sendmsg(dev->socket, &msg);

    return ret == len ? NETDEV_TX_OK : NETDEV_TX_BUSY;
}

/*****************************************************************************/

/** Polls the device.
 */

// delegate to some rtdm thread when called from nrt context
struct recvmsg_params {
    int socket;
    struct user_msghdr *msg;
};

static int recvmsg_handler(struct rt_proc_call *call)
{
    struct recvmsg_params *params;

    params = rtpc_get_priv(call, struct recvmsg_params);

    return  rtdm_recvmsg(params->socket, params->msg, MSG_DONTWAIT);
}

static ssize_t
nrt_rtdm_recvmsg(int socket, struct user_msghdr *msg)
{
    int ret;
    struct recvmsg_params params = {socket,msg};

    ret = rtpc_dispatch_call(recvmsg_handler, 0, &params,
                             sizeof(params), NULL, NULL);
    return ret;
}

void ec_gen_device_poll(
        ec_gen_device_t *dev
        )
{
    struct user_msghdr msg;
    struct iovec iov;
    int ret, budget = 128; // FIXME

    ecdev_set_link(dev->ecdev, rtnetif_carrier_ok(dev->used_netdev));

    do {
        iov.iov_base = dev->rx_buf;
        iov.iov_len = EC_GEN_RX_BUF_SIZE;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov     = &iov;
        msg.msg_iovlen  = 1;

        if (rtdm_in_rt_context()){
            /* HACK : call fd ops directly as in rtdm's fd.c */
            ret = dev->rtdm_fd->ops->recvmsg_rt(dev->rtdm_fd, &msg, MSG_DONTWAIT);
        }else
            ret = nrt_rtdm_recvmsg(dev->socket, &msg);

        if (ret > 0) {
            ecdev_receive(dev->ecdev, dev->rx_buf, ret);
        } else if (ret < 0) {
            break;
        }
        budget--;
    } while (budget);
}

/*****************************************************************************/

/** Offer device.
 */
int offer_device(
        ec_gen_interface_desc_t *desc
        )
{
    ec_gen_device_t *gendev;
    int ret = 0;

    gendev = kmalloc(sizeof(ec_gen_device_t), GFP_KERNEL);
    if (!gendev) {
        return -ENOMEM;
    }

    ret = ec_gen_device_init(gendev);
    if (ret) {
        kfree(gendev);
        return ret;
    }

    if (ec_gen_device_offer(gendev, desc)) {
        list_add_tail(&gendev->list, &generic_devices);
    } else {
        ec_gen_device_clear(gendev);
        kfree(gendev);
    }

    return ret;
}

/*****************************************************************************/

/** Clear devices.
 */
void clear_devices(void)
{
    ec_gen_device_t *gendev, *next;

    list_for_each_entry_safe(gendev, next, &generic_devices, list) {
        list_del(&gendev->list);
        ec_gen_device_clear(gendev);
        kfree(gendev);
    }
}

/*****************************************************************************/

/** Module initialization.
 *
 * Initializes \a master_count masters.
 * \return 0 on success, else < 0
 */
int __init ec_gen_init_module(void)
{
    int ret = 0;
    int devices = 0;
    int i;
    struct list_head descs;
    struct rtnet_device *rtdev;
    ec_gen_interface_desc_t *desc, *next;

    printk(KERN_INFO PFX "EtherCAT master RTnet Ethernet device module %s\n",
            EC_MASTER_VERSION);

    INIT_LIST_HEAD(&generic_devices);
    INIT_LIST_HEAD(&descs);

    for (i = 0; i < MAX_RT_DEVICES; i++) {

        rtdev = rtdev_get_by_index(i);
        if (rtdev != NULL) {
            mutex_lock(&rtdev->nrt_lock);

            //if (test_bit(PRIV_FLAG_UP, &rtdev->priv_flags)) {
            //    mutex_unlock(&rtdev->nrt_lock);
            //    printk(KERN_ERR PFX "%s busy, skipping device!\n", rtdev->name);
            //    rtdev_dereference(rtdev);
            //    continue;
            //}

            desc = kmalloc(sizeof(ec_gen_interface_desc_t), GFP_ATOMIC);
            if (!desc) {
                ret = -ENOMEM;
                goto out_err;
            }
            strncpy(desc->name, rtdev->name, IFNAMSIZ);
            desc->netdev = rtdev;
            desc->ifindex = rtdev->ifindex;
            memcpy(desc->dev_addr, rtdev->dev_addr, ETH_ALEN);
            list_add_tail(&desc->list, &descs);
              mutex_unlock(&rtdev->nrt_lock);

            devices++;
        }
    }

    if (devices == 0) {
        printk(KERN_ERR PFX "no real-time devices found!\n");
        ret = -ENODEV;
        goto out_err;
    }

    list_for_each_entry_safe(desc, next, &descs, list) {
        ret = offer_device(desc);
        if (ret) {
            goto out_err;
        }
        kfree(desc);
    }
    return ret;

out_err:
    list_for_each_entry_safe(desc, next, &descs, list) {
        list_del(&desc->list);
        kfree(desc);
    }
    clear_devices();
    return ret;
}

/*****************************************************************************/

/** Module cleanup.
 *
 * Clears all master instances.
 */
void __exit ec_gen_cleanup_module(void)
{
    clear_devices();
    printk(KERN_INFO PFX "Unloading.\n");
}

/*****************************************************************************/

/** \cond */

module_init(ec_gen_init_module);
module_exit(ec_gen_cleanup_module);

/** \endcond */

/*****************************************************************************/
