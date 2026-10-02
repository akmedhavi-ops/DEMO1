/* 40vxredund.cdf - VxWorks 7 component description (TEMPLATE) */

Component INCLUDE_VXREDUND {
    NAME        vxredund redundancy middleware
    SYNOPSIS    Active/standby state replication with bounded failover time
    ARCHIVE     libvxredund.a
    LINK_SYMS   rdn_create rdn_link_udp_open rdn_link_vme_open
    REQUIRES    INCLUDE_SEM_BINARY INCLUDE_SEM_MUTEX
    _CHILDREN   FOLDER_NOT_VISIBLE
}
