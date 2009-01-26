/******************************************************************************
 *  
 *  $Id$
 * 
 *  Copyright (C) 2006-2009  Florian Pose, Ingenieurgemeinschaft IgH
 *  
 *  This file is part of the IgH EtherCAT master userspace library.
 *  
 *  The IgH EtherCAT master userspace library is free software; you can
 *  redistribute it and/or modify it under the terms of the GNU Lesser General
 *  Public License as published by the Free Software Foundation; version 2.1
 *  of the License.
 *
 *  The IgH EtherCAT master userspace library is distributed in the hope that
 *  it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 *  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with the IgH EtherCAT master userspace library. If not, see
 *  <http://www.gnu.org/licenses/>.
 *  
 *  Using the EtherCAT technology and brand is permitted in compliance with
 *  the industrial property and similar rights of Beckhoff Automation GmbH.
 *
 *****************************************************************************/

#include "include/ecrt.h"

/*****************************************************************************/

struct ec_slave_config {
    ec_master_t *master;
    unsigned int index;
    uint16_t alias;
    uint16_t position;
};

/*****************************************************************************/
