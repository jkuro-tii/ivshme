/* Copyright 2022-2023 TII (SSRC) and the Ghaf contributors
   SPDX-License-Identifier: Apache-2.0
*/

#define SHMEM_IOC_MAGIC 's'

#define SHMEM_IOCWLOCAL     _IOR(SHMEM_IOC_MAGIC, 1, int)
#define SHMEM_IOCWREMOTE    _IOR(SHMEM_IOC_MAGIC, 2, int)
#define SHMEM_IOCIVPOSN     _IOW(SHMEM_IOC_MAGIC, 3, int)
#define SHMEM_IOCDORBELL    _IOR(SHMEM_IOC_MAGIC, 4, int)
#define SHMEM_IOCRESTART    _IOR(SHMEM_IOC_MAGIC, 5, int)
#define SHMEM_IOCSETPEERID  _IOR(SHMEM_IOC_MAGIC, 6, int)
#define SHMEM_IOCNOP        _IOR(SHMEM_IOC_MAGIC, 7, int)

