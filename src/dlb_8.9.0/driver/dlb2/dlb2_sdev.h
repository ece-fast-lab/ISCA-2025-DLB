/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright(c) 2017-2020 Intel Corporation
 */

int dlb2_sdev_configure(struct pci_dev *pdev, int num_sdevs);
int dlb2_sdev_create(struct dlb2 *dlb2, struct pci_dev *pdev, int sdev_id);
void dlb2_sdev_destroy(struct dlb2 *dlb2);
struct dlb2 *dlb2_init_sdev(struct dlb2 *dlb2, int sdev_id);

int dlb2_pf_query_cq_poll_mode(struct dlb2 *dlb2,
                           struct dlb2_cmd_response *user_resp);
int dlb2_pf_enable_ldb_cq_interrupts(struct dlb2 *dlb2,
                                 int domain_id,
                                 int id,
                                 u16 thresh);
int dlb2_pf_enable_dir_cq_interrupts(struct dlb2 *dlb2,
                                 int domain_id,
                                 int id,
                                 u16 thresh);
int dlb2_pf_arm_cq_interrupt(struct dlb2 *dlb2,
                         int domain_id,
                         int port_id,
                         bool is_ldb);
