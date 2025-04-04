/*
--------------------------------------------------------------------------------
This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.
This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.
You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
Boston, MA  02110-1301, USA.
--------------------------------------------------------------------------------
*/

// Copyright (c) 2015-2016 John Seamons, ZL4VO/KF6VO

#include "types.h"
#include "config.h"
#include "kiwi.h"
#include "system.h"
#include "services.h"
#include "rx.h"
#include "rx_util.h"
#include "mem.h"
#include "misc.h"
#include "str.h"
#include "nbuf.h"
#include "web.h"
#include "net.h"
#include "peri.h"
#include "gps_.h"
#include "coroutines.h"
#include "debug.h"
#include "printf.h"
#include "cfg.h"
#include "clk.h"
#include "dx.h"
#include "wspr.h"
#include "FT8.h"

#include "data_pump.h"
#include "ext_int.h"

#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <math.h>
#include <limits.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>

#include <sys/prctl.h>
#include <pty.h>

typedef struct {
    bool have_pushback;
    char produce[32], consume[32];
} pushback_t;

static pushback_t pushback;

void c2s_admin_setup(void* param) {
    conn_t* conn = (conn_t*)param;

    // send initial values
    memset(&pushback, 0, sizeof(pushback));
    send_msg(conn, SM_NO_DEBUG, "ADM admin_sdr_mode=%d", 1);
    const char* proxy_server = admcfg_string("proxy_server", NULL, CFG_REQUIRED);
    send_msg_encoded(conn, "ADM", "proxy_url", "%s:%d", proxy_server, PROXY_SERVER_PORT);
    admcfg_string_free(proxy_server);
    send_msg(conn, SM_NO_DEBUG, "ADM init=%d", rx_chans);
}

void c2s_admin_shutdown(void* param) {
    conn_t* c = (conn_t*)param;
    int r;

    if (c->console_child_pid != 0) {
        r = kill(c->console_child_pid, 0); // see if child is still around
        if (r < 0) {
            // cprintf(c, "CONSOLE: no child pid %d? errno=%d (%s)\n", c->console_child_pid, errno, strerror(errno));
        }
        else {
            if (c->master_pty_fd > 0) {
                close(c->master_pty_fd);
                c->master_pty_fd = 0;
            }
            scall("console child", kill(c->console_child_pid, SIGKILL));
        }
        c->console_child_pid = 0;
    }

    free(c->oob_buf); // okay if c->oob_buf == NULL
}

// tunnel task
static void tunnel(void* param) {
    conn_t* c = (conn_t*)param;

    char* tname;
    asprintf(&tname, "tunnel[%02d]", c->self_idx);
    TaskNameSFree(tname);
    clprintf(c, "TUNNEL: open connection\n");

#define PIPE_R 0
#define PIPE_W 1
    int si[2], so[2];
    scall("pipeSI", pipe(si));
    scall("pipeSO", pipe(so));

    pid_t child_pid;
    scall("fork", (child_pid = fork()));

    if (child_pid == 0) {
        // terminate when parent exits
        scall("PR_SET_PDEATHSIG", prctl(PR_SET_PDEATHSIG, SIGTERM));

        scall("dupSI", dup2(si[PIPE_R], STDIN_FILENO));
        scall("closeSI", close(si[PIPE_W]));
        scall("closeSO", close(so[PIPE_R]));
        scall("dupSO", dup2(so[PIPE_W], STDOUT_FILENO));
        system("/usr/sbin/sshd -D -p 1138 >/dev/null 2>&1");
        scall("execl", execl("/bin/nc", "/bin/nc", "localhost", "1138", (char*)NULL));
        child_exit(EXIT_FAILURE);
    }

#if 0
	    // technically a race here between finding and using the free port
        int sock;
        scall("socket", (sock = socket(AF_INET, SOCK_STREAM, 0)));
        struct sockaddr_in sa;
        socklen_t len = sizeof(sa);
        memset(&sa, 0, len);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons(0);
        scall("bind", bind(sock, (struct sockaddr*) &sa, len));
        scall("getsockname", getsockname(sock, (struct sockaddr*) &sa, &len));
        int port = ntohs(sa.sin_port);
        printf("port=%d\n", port);
        close(sock);
        system(stprintf("/usr/sbin/sshd -D -p %d >/dev/null 2>&1", port));
#endif
}

// console task
static void console_task(void* param) {
    conn_t* c = (conn_t*)param;

    char* tname;
    asprintf(&tname, "console[%02d]", c->self_idx);
    TaskNameSFree(tname);
    cprintf(c, "CONSOLE: open connection\n");
    send_msg_encoded(c, "ADM", "console_c2w", "CONSOLE: open connection\n");

#define NBUF 1024
    char* buf = (char*)kiwi_imalloc("console", NBUF + SPACE_FOR_NULL);
    int i, n, err;

    char* args[] = { (char*)"/bin/sh", (char*)"--login", NULL };
    scall("forkpty", (c->console_child_pid = forkpty(&c->master_pty_fd, NULL, NULL, NULL)));

    if (c->console_child_pid == 0) { // child
        // terminate when parent exits
        scall("PR_SET_PDEATHSIG", prctl(PR_SET_PDEATHSIG, SIGTERM));

        execve(args[0], args, NULL);
        child_exit(EXIT_SUCCESS);
    }

    register_zombie(c->console_child_pid);
    scall("", fcntl(c->master_pty_fd, F_SETFL, O_NONBLOCK));

    /*
    // remove the echo
    struct termios tios;
    tcgetattr(c->master_pty_fd, &tios);
    tios.c_lflag &= ~(ECHO | ECHONL);
    tcsetattr(c->master_pty_fd, TCSAFLUSH, &tios);
    */

    // printf("master_pty_fd=%d\n", c->master_pty_fd);

    do {
        TaskSleepMsec(250); // can be woken up prematurely by console_oob_key

        // Without this a reload of the admin console page with an active shell often
        // hangs in the read() below even though O_NONBLOCK has been set on the fd!
        if (c->mc == NULL)
            break;

        n = read(c->master_pty_fd, buf, NBUF);
        // real_printf("n=%d errno=%d\n", n, errno);
        if (n > 0 && c->mc) {
            buf[n] = '\0';

            // FIXME: why, when we write > 50 chars to the shell input, does the echoed
            // output get mangled with multiple STX (0x02) characters?
            // real_printf("read %d %d >>>%s<<<\n", n, strlen(buf), kiwi_str_ASCII_static(buf));

            // UTF-8 end-of-buffer fragmentation possibilities:
            //
            // NN = 0xxx_xxxx 0x00-0x7f non-encoded
            // CC = 10xx_xxxx 0x80-0xb3 continuation byte
            // LL = 11xx_xxxx 0xc0-0xff leading byte
            //    L1 = 110x_xxxx %c0-%df [CC]
            //    L2 = 1110_xxxx %e0-%ef [CC] [CC]
            //    L3 = 1111_0xxx %f0-%f7 [CC] [CC] [CC]
            //
            // 987 654 321    [len-N]
            //  c8  c5  c2
            //         %LL    i.e. %L1 or %L2 or %L3
            //     %L2 %CC
            //     %L3 %CC
            // %L3 %CC %CC

            bool do_pushback = false;
            char* cp = &buf[n - 1];
            while ((u1_t)*cp >= 0x80) {
                do_pushback = true;
                if ((u1_t)*cp >= 0xc0 || cp == buf) break;
                cp--;
            }
            if (cp == buf) do_pushback = false;

            if (do_pushback) {
                strcpy(pushback.produce, cp);
                // real_printf("pushback PRODUCE %d <%s>\n", strlen(pushback.produce), kiwi_str_ASCII_static(pushback.produce));
                *cp = '\0';
                pushback.have_pushback = true;
            }

            // real_printf("console_c2w %d <%s> %d <%s>\n", strlen(pushback.consume), kiwi_str_ASCII_static(pushback.consume, 0), strlen(buf), kiwi_str_ASCII_static(buf, 1));
            send_msg_encoded(c, "ADM", "console_c2w", "%s%s", pushback.consume, buf);

            if (pushback.have_pushback) {
                strcpy(pushback.consume, pushback.produce);
                pushback.have_pushback = false;
            }
            else {
                pushback.consume[0] = '\0';
            }
        }

        // process out-of-band chars
        // multi-char sequences (e.g. arrow keys) are sent via "console_w2c="
        while (c->oob_buf != NULL && c->oob_r != c->oob_w) {
            u1_t ch = c->oob_buf[c->oob_r];
            n = write(c->master_pty_fd, &ch, 1);
            // printf("sent console_oob_key ch=%-3s (%02x) n=%d\n", ASCII[ch], ch, n);
            c->oob_r++;
            if (c->oob_r == N_OOB_BUF) c->oob_r = 0;
        }

    } while ((n > 0 || (n == -1 && errno == EAGAIN)) && c->mc);

    if (n < 0 /*&& errno != EIO*/ && c->mc) {
        cprintf(c, "CONSOLE: n=%d errno=%d (%s)\n", n, errno, strerror(errno));
    }
    if (c->master_pty_fd > 0)
        close(c->master_pty_fd);
    kiwi_ifree(buf, "console");
    c->master_pty_fd = 0;
    c->console_child_pid = 0;

    if (c->mc) {
        send_msg_encoded(c, "ADM", "console_c2w", "CONSOLE: exited\n");
        send_msg(c, SM_NO_DEBUG, "ADM console_done");
    }

#undef NBUF
}

bool DUC_enable_start, rev_enable_start;

void c2s_admin(void* param) {
    int i, j, k, n, rv, status;
    conn_t* conn = (conn_t*)param;
    rx_common_init(conn);
    char *sb, *sb2;
    char *cmd_p, *buf_m;

    nbuf_t* nb = NULL;
    while (TRUE) {

        // rv = waitpid(-1, NULL, WNOHANG);
        // if (rv) real_printf("c2s_admin CULLED %d\n", rv);

        if (nb) web_to_app_done(conn, nb);
        n = web_to_app(conn, &nb);

        if (n) {
            char* cmd = nb->buf;
            cmd[n] = 0; // okay to do this -- see nbuf.c:nbuf_allocq()

            TaskStat(TSTAT_INCR | TSTAT_ZERO, 0, "cmd");

//#define ADMIN_TUNNEL
#ifdef ADMIN_TUNNEL
            // printf("ADMIN: auth=%d mc=%p %d <%s>\n", conn->auth, conn->mc, strlen(cmd), cmd);

            // SECURITY: tunnel commands allowed/required before auth check in rx_common_cmd()
            if (strcmp(cmd, "ADM tunO") == 0) {
                cprintf(conn, "tunO\n");
                continue;
            }

            if (strncmp(cmd, "ADM tunW ", 9) == 0) {
                cprintf(conn, "tunW <%s>\n", &cmd[9]);
                continue;
            }
#endif

            // SECURITY: this must be first for auth check
            if (rx_common_cmd(STREAM_ADMIN, conn, cmd))
                continue;

                // printf("ADMIN: %d <%s>\n", strlen(cmd), cmd);

#ifdef ADMIN_TUNNEL
            if (conn->auth != true || conn->auth_admin != true) {
                clprintf(conn, "### SECURITY: NO ADMIN CONN AUTH YET: %d %d %d %s <%s>\n",
                         conn->auth, conn->auth_admin, conn->remote_ip, cmd);
                continue;
            }
#else
            assert(conn->auth == true);       // auth completed
            assert(conn->auth_admin == true); // auth as admin
#endif

            i = strcmp(cmd, "SET init");
            if (i == 0) {
                continue;
            }


            ////////////////////////////////
            // status
            ////////////////////////////////

            i = strcmp(cmd, "SET dpump_hist_reset");
            if (i == 0) {
                dpump.force_reset = true;
                dpump.resets = 0;
                continue;
            }


            ////////////////////////////////
            // control
            ////////////////////////////////

            int server_enabled;
            i = sscanf(cmd, "SET server_enabled=%d", &server_enabled);
            if (i == 1) {
                clprintf(conn, "ADMIN: server_enabled=%d\n", server_enabled);

                if (server_enabled) {
                    down = false;
                }
                else {
                    down = true;
                    rx_server_kick(KICK_USERS); // kick all users off
                }
                continue;
            }

            int chan;
            i = sscanf(cmd, "SET user_kick=%d", &chan);
            if (i == 1) {
                rx_server_kick(KICK_CHAN, chan);
                continue;
            }

            if (strcmp(cmd, "SET snr_meas") == 0) {
                if (SNR_meas_tid) {
                    TaskWakeupF(SNR_meas_tid, TWF_CANCEL_DEADLINE);
                }
                continue;
            }

            int dload;
            i = sscanf(cmd, "SET dx_comm_download=%d", &dload);
            if (i == 1) {
                if (dload == 1) {
                    system("touch " DX_DOWNLOAD_ONESHOT_FN);
                }
                else {
                    system("rm -f " DX_DOWNLOAD_ONESHOT_FN);
                }
                clprintf(conn, "ADMIN: dx_comm_download %s\n", dload ? "NOW" : "CANCEL");
                continue;
            }


            ////////////////////////////////
            // connect
            ////////////////////////////////


            // DUC

            i = strcmp(cmd, "SET DUC_status_query");
            if (i == 0) {
                if (DUC_enable_start) {
                    send_msg(conn, SM_NO_DEBUG, "ADM DUC_status=301");
                    net.DUC_status = 301;
                }
                continue;
            }

            // FIXME: hardwired to eth0 -- needs to support wlans
            char* args_m = NULL;
            n = sscanf(cmd, "SET DUC_start args=%256ms", &args_m);
            if (n == 1) {
                kiwi_str_decode_inplace(args_m);
                asprintf(&cmd_p, "/usr/bin/noip2 -C -c " DIR_CFG "/noip2.conf %s -I eth0 2>&1", args_m);
                kiwi_asfree(args_m);
                printf("DUC: %s\n", cmd_p);
                char* reply;
                int stat;
                reply = non_blocking_cmd(cmd_p, &stat);
                kiwi_asfree(cmd_p);
                if (stat < 0 || n <= 0) {
                    lprintf("DUC: noip2 failed?\n");
                    send_msg(conn, SM_NO_DEBUG, "ADM DUC_status=300");
                    net.DUC_status = 300;
                    continue;
                }
                status = WEXITSTATUS(stat);
                printf("DUC: status=%d\n", status);
                printf("DUC: <%s>\n", kstr_sp(reply));
                kstr_free(reply);
                send_msg(conn, SM_NO_DEBUG, "ADM DUC_status=%d", status);
                net.DUC_status = status;
                if (status != 0) continue;
                DUC_enable_start = true;

                system("/etc/init.d/noip2 restart");

                continue;
            }


            // proxy

            i = strcmp(cmd, "SET rev_status_query");
            if (i == 0) {
                net.proxy_status = rev_enable_start ? 200 : 201;
                send_msg(conn, SM_NO_DEBUG, "ADM rev_status=%d", net.proxy_status);
                continue;
            }

            char *user_m = NULL, *host_m = NULL;
            n = sscanf(cmd, "SET rev_register user=%256ms host=%256ms", &user_m, &host_m);
            if (n == 2) {
                const char* proxy_server = admcfg_string("proxy_server", NULL, CFG_REQUIRED);

                send_msg(conn, SM_NO_DEBUG, "ADM rev_status=%d", status);
                net.proxy_status = 0;

                asprintf(&cmd_p, "sed -e s/SERVER/%s/ -e s/USER/%s/ -e s/HOST/%s/ -e s/PORT/%d/ %s >%s",
                         proxy_server, user_m, host_m, net.port_ext, DIR_CFG "/frpc.template.ini", DIR_CFG "/frpc.toml");
                printf("proxy register: %s\n", cmd_p);
                system(cmd_p);
                kiwi_asfree(cmd_p);
                kiwi_asfree(user_m);
                kiwi_asfree(host_m);
                admcfg_string_free(proxy_server);

                // copy config file to sd card
                sd_enable(true);
                system("cp -u /root/config/* /media/mmcblk0p1/config/");
                sd_enable(false);

                system("/etc/init.d/frpc restart");

                continue;
            }
            else if (n == 1) {
                kiwi_asfree(user_m);
            }

            int ov_counts;
            i = sscanf(cmd, "SET ov_counts=%d", &ov_counts);
            if (i == 1) {
                // adjust ADC overload detect count mask
                u4_t ov_counts_mask = (~(ov_counts - 1)) & 0xffff;
                // printf("ov_counts_mask %d 0x%x\n", ov_counts, ov_counts_mask);
                fpga_setovmask(ov_counts_mask);
                continue;
            }


            ////////////////////////////////
            // channels [not used currently]
            ////////////////////////////////


            ////////////////////////////////
            // webpage
            ////////////////////////////////

            i = strcmp(cmd, "SET reload_index_params");
            if (i == 0) {
                printf("reload_index_params\n");
                reload_index_params();
                continue;
            }


            ////////////////////////////////
            // public
            ////////////////////////////////

            i = strcmp(cmd, "SET public_wakeup");
            if (i == 0) {
                wakeup_reg_kiwisdr_com(WAKEUP_REG);
                continue;
            }

            ////////////////////////////////
            // adc
            ////////////////////////////////

            int adc_dither;
            i = sscanf(cmd, "SET dither=%d", &adc_dither);
            if (i == 1) {
                fpga_set_dither(adc_dither);
                continue;
            }

            int adc_pga;
            i = sscanf(cmd, "SET pga=%d", &adc_pga);
            if (i == 1) {
                fpga_set_pga(adc_pga);
                continue;
            }

            ////////////////////////////////
            // dx
            ////////////////////////////////


            ////////////////////////////////
            // update
            ////////////////////////////////

            int force_check, force_build;
            i = sscanf(cmd, "SET force_check=%d force_build=%d", &force_check, &force_build);
            if (i == 2) {
                check_for_update(force_build ? FORCE_BUILD : FORCE_CHECK, conn);
                continue;
            }


////////////////////////////////
// backup
////////////////////////////////
#if 0
			i = strcmp(cmd, "SET microSD_write");
			if (i == 0) {
				mprintf_ff("ADMIN: received microSD_write\n");
			    sd_backup(conn, true);
				continue;
			}
#endif

            ////////////////////////////////
            // network
            ////////////////////////////////

            i = strcmp(cmd, "SET auto_nat_status_poll");
            if (i == 0) {
                send_msg(conn, SM_NO_DEBUG, "ADM auto_nat=%d", net.auto_nat);
                continue;
            }

            i = strcmp(cmd, "SET auto_nat_set");
            if (i == 0) {
                cprintf(conn, "auto NAT: auto_nat_set\n");
                UPnP_port(NAT_DELETE);
                continue;
            }

            i = strcmp(cmd, "SET check_port_open");
            if (i == 0) {
                const char* server_url = cfg_string("server_url", NULL, CFG_OPTIONAL);
                // proxy always uses a fixed port number
                int dom_sel = cfg_int("sdr_hu_dom_sel", NULL, CFG_REQUIRED);
                int server_port = (dom_sel == DOM_SEL_REV) ? PROXY_SERVER_PORT : net.port_ext;
                int status;
                char* reply;
                asprintf(&cmd_p, "www.rx-888.com/api/check_port_open?url=%s:%d", server_url, server_port);
                reply = curl_get(cmd_p, 15, &status);
                printf("check_port_open: %s\n", cmd_p);
                kiwi_asfree(cmd_p);
                if (reply == NULL || status < 0 || WEXITSTATUS(status) != 0) {
                    printf("check_port_open: ERROR %p 0x%x\n", reply, status);
                    status = -2;
                }
                else {
                    char* rp = kstr_sp(reply);
                    printf("check_port_open: <%s>\n", rp);
                    status = -1;
                    /* n = */ sscanf(rp, "status=%d", &status);
                    // printf("check_port_open: n=%d status=0x%02x\n", n, status);
                }
                kstr_free(reply);
                cfg_string_free(server_url);
                send_msg(conn, SM_NO_DEBUG, "ADM check_port_status=%d", status);
                continue;
            }

            // FIXME: support wlan0
            char *static_ip_m = NULL, *static_nm_m = NULL, *static_gw_m = NULL;
            int static_nb;
            i = sscanf(cmd, "SET static_ip=%32ms static_nb=%d static_nm=%32ms static_gw=%32ms", &static_ip_m, &static_nb, &static_nm_m, &static_gw_m);
            if (i == 4) {
                clprintf(conn, "eth0: USE STATIC ip=%s nm=%s(%d) gw=%s\n", static_ip_m, static_nm_m, static_nb, static_gw_m);

                system("cp /etc/network/interfaces /etc/network/interfaces.bak");
                FILE* fp;
                scallz("/tmp/interfaces.kiwi fopen", (fp = fopen("/tmp/interfaces.kiwi", "w")));
                fprintf(fp, "auto lo\n");
                fprintf(fp, "iface lo inet loopback\n");
                fprintf(fp, "auto eth0\n");
                fprintf(fp, "iface eth0 inet static\n");
                fprintf(fp, "    address %s\n", static_ip_m);
                fprintf(fp, "    netmask %s\n", static_nm_m);
                fprintf(fp, "    gateway %s\n", static_gw_m);
                fprintf(fp, "iface wlan0 inet static\n");
                fprintf(fp, "    address 192.168.7.2\n");
                fprintf(fp, "    netmask 255.255.255.252\n");
                fprintf(fp, "    network 192.168.7.0\n");
                fprintf(fp, "    gateway 192.168.7.1\n");
                fclose(fp);
                system("cp /tmp/interfaces.kiwi /etc/network/interfaces");
                system("rm /etc/network/interfaces.bak");
                system("lbu commit -d");

                kiwi_asfree(static_ip_m);
                kiwi_asfree(static_nm_m);
                kiwi_asfree(static_gw_m);
                continue;
            }
            kiwi_asfree(static_ip_m);
            kiwi_asfree(static_nm_m);
            kiwi_asfree(static_gw_m);

            char *dns1_m = NULL, *dns2_m = NULL;
            i = strncmp(cmd, "SET dns", 7);
            if (i == 0) {
                i = sscanf(cmd, "SET dns dns1=%32ms dns2=%32ms", &dns1_m, &dns2_m);
                if (i == 2) {
                    kiwi_str_decode_inplace(dns1_m);
                    kiwi_str_decode_inplace(dns2_m);
                    char* dns1 = (dns1_m[0] == 'x') ? (dns1_m + 1) : dns1_m;
                    char* dns2 = (dns2_m[0] == 'x') ? (dns2_m + 1) : dns2_m;
                    clprintf(conn, "SET dns1=%s dns2=%s\n", dns1, dns2);

                    bool dns1_err, dns2_err;
                    inet4_d2h(dns1, &dns1_err);
                    inet4_d2h(dns2, &dns2_err);

                    if (!dns1_err || !dns2_err) {
                        system("rm -f /etc/resolv.conf; touch /etc/resolv.conf");

                        if (!dns1_err) {
                            asprintf(&sb, "echo nameserver %s >> /etc/resolv.conf", dns1);
                            system(sb);
                            kiwi_asfree(sb);
                        }

                        if (!dns2_err) {
                            asprintf(&sb, "echo nameserver %s >> /etc/resolv.conf", dns2);
                            system(sb);
                            kiwi_asfree(sb);
                        }
                    }

                    kiwi_asfree(dns1_m);
                    kiwi_asfree(dns2_m);
                    continue;
                }

                kiwi_asfree(dns1_m);
                kiwi_asfree(dns2_m);
            }

            // FIXME: support wlan0
            i = strcmp(cmd, "SET use_DHCP");
            if (i == 0) {
                clprintf(conn, "eth0: USE DHCP\n");
                system("rm /etc/network/interfaces");
                system("touch /etc/network/interfaces");
                system("lbu commit -d");
                continue;
            }

            i = strcmp(cmd, "SET network_ip_blacklist_clear");
            if (i == 0) {
                cprintf(conn, "\"iptables -D INPUT -j KIWI; iptables -F KIWI; iptables -X KIWI; iptables -N KIWI\"\n");
                system("iptables -D INPUT -j KIWI; iptables -F KIWI; iptables -X KIWI; iptables -N KIWI");

                net.ip_blacklist_len = 0;
                continue;
            }

            char* ip_m = NULL;
            i = sscanf(cmd, "SET network_ip_blacklist=%64ms", &ip_m);
            if (i == 1) {
                kiwi_str_decode_inplace(ip_m);
                // printf("network_ip_blacklist %s\n", ip_m);
                rv = ip_blacklist_add_iptables(ip_m);
                send_msg_encoded(conn, "ADM", "network_ip_blacklist_status", "%d,%s", rv, ip_m);
                kiwi_asfree(ip_m);
                continue;
            }

            i = strcmp(cmd, "SET network_ip_blacklist_enable");
            if (i == 0) {
                cprintf(conn, "\"iptables -A KIWI -j RETURN; iptables -A INPUT -j KIWI\"\n");
                system("iptables -A KIWI -j RETURN; iptables -A INPUT -j KIWI");
                send_msg(conn, SM_NO_DEBUG, "ADM network_ip_blacklist_enabled");
                continue;
            }


            ////////////////////////////////
            // GPS
            ////////////////////////////////

            n = strcmp(cmd, "SET gps_az_el_history");
            if (n == 0) {
                lock_holder holder(gps_lock);

                int now;
                utc_hour_min_sec(NULL, &now);

                int az, el;
                int sat_seen[MAX_SATS], prn_seen[MAX_SATS], samp_seen[AZEL_NSAMP];
                memset(sat_seen, 0, sizeof(sat_seen));
                memset(prn_seen, 0, sizeof(prn_seen));
                memset(samp_seen, 0, sizeof(samp_seen));

                // sat/prn seen during any sample period
                int nsats = 0;
                for (int sat = 0; sat < MAX_SATS; sat++) {
                    for (int samp = 0; samp < AZEL_NSAMP; samp++) {
                        if (gps.el[samp][sat] != 0) {
                            sat_seen[sat] = sat + 1; // +1 bias
                            prn_seen[sat] = sat + 1; // +1 bias
                            break;
                        }
                    }
                    if (sat_seen[sat]) nsats++;
                }

#if 0
                if (gps_debug) {
                    // any sat/prn seen during specific sample period
                    for (int samp = 0; samp < AZEL_NSAMP; samp++) {
                        for (int sat = 0; sat < MAX_SATS; sat++) {
                            if (gps.el[samp][sat] != 0) {
                                samp_seen[samp] = 1;
                                break;
                            }
                        }
                    }
        
                    real_printf("-----------------------------------------------------------------------------\n");
                    for (int samp = 0; samp < AZEL_NSAMP; samp++) {
                        if (!samp_seen[samp] && samp != now) continue;
                        for (int sat = 0; sat < MAX_SATS; sat++) {
                            if (!sat_seen[sat]) continue;
                            real_printf("%s     ", PRN(prn_seen[sat]-1));
                        }
                        real_printf("SAMP %2d %s\n", samp, (samp == now)? "==== NOW ====":"");
                        for (int sat = 0; sat < MAX_SATS; sat++) {
                            if (!sat_seen[sat]) continue;
                            az = gps.az[samp][sat];
                            el = gps.el[samp][sat];
                            if (az == 0 && el == 0)
                                real_printf("         ");
                            else
                                real_printf("%3d|%2d   ", az, el);
                        }
                        real_printf("\n");
                    }
                }
#endif

                // send history only for sats seen
                sb = kstr_asprintf(NULL, "{\"n_sats\":%d,\"n_samp\":%d,\"now\":%d,", MAX_SATS, AZEL_NSAMP, now);
                sb = kstr_cat(sb, kstr_list_int("\"sat_seen\":[", "%d", "],\"prn_seen\":[", sat_seen, MAX_SATS, sat_seen, -1)); // -1 bias

                int first = 1;
                for (int sat = 0; sat < MAX_SATS; sat++) {
                    if (!sat_seen[sat]) continue;
                    // const char *prn_s = "N";
                    // char *prn_s = PRN(prn_seen[sat]-1);
                    // if (*prn_s == 'N') prn_s++;
                    const char* prn_s = Sats[sat].prn_s;
                    sb = kstr_asprintf(sb, "%s\"%s\"", first ? "" : ",", prn_s);
                    first = 0;
                }
                sb = kstr_cat(sb, "],\"az\":[");

                if (nsats)
                    for (int samp = 0; samp < AZEL_NSAMP; samp++) {
                        sb = kstr_cat(sb, kstr_list_int(samp ? "," : "", "%d", "", gps.az[samp], MAX_SATS, sat_seen));
                    }

                NextTask("gps_az_el_history1");

                sb = kstr_cat(sb, "],\"el\":[");
                if (nsats)
                    for (int samp = 0; samp < AZEL_NSAMP; samp++) {
                        sb = kstr_cat(sb, kstr_list_int(samp ? "," : "", "%d", "", gps.el[samp], MAX_SATS, sat_seen));
                    }

                sb = kstr_asprintf(sb, "],\"qzs3\":{\"az\":%d,\"el\":%d},", gps.qzs_3.az, gps.qzs_3.el);
                sb = kstr_cat(sb, kstr_list_int("\"shadow_map\":[", "%u", "]}", (int*)gps.shadow_map, 360));

                send_msg_encoded(conn, "MSG", "gps_az_el_history_cb", "%s", kstr_sp(sb));
                kstr_free(sb);
                NextTask("gps_az_el_history2");
                continue;
            }

            n = strcmp(cmd, "SET gps_update");
            if (n == 0) {
                // sends a list of the last gps.POS_len entries per query
                if (gps.POS_seq_w != gps.POS_seq_r) {
                    sb = kstr_asprintf(NULL, "{\"ref_lat\":%.6f,\"ref_lon\":%.6f,\"POS\":[", gps.sgnLat, gps.sgnLon);
                    int xmax, xmin, ymax, ymin;
                    xmax = ymax = INT_MIN;
                    xmin = ymin = INT_MAX;
                    for (k = 0; k < gps.POS_len; k++) {
                        sb = kstr_asprintf(sb, "%s%.6f,%.6f", k ? "," : "", gps.POS_data[k].lat, gps.POS_data[k].lon);
                        if (gps.POS_data[k].lat != 0) {
                            int x = gps.POS_data[k].x;
                            if (x > xmax)
                                xmax = x;
                            else if (x < xmin)
                                xmin = x;
                            int y = gps.POS_data[k].y;
                            if (y > ymax)
                                ymax = y;
                            else if (y < ymin)
                                ymin = y;
                        }
                    }

                    sb = kstr_asprintf(sb, "],\"xspan\":%d,\"yspan\":%d}",
                                       xmax - xmin, ymax - ymin);
                    send_msg_encoded(conn, "MSG", "gps_POS_data_cb", "%s", kstr_sp(sb));
                    kstr_free(sb);
                    gps.POS_seq_r = gps.POS_seq_w;
                    NextTask("gps_update2");
                }

                gps_chan_t* c;

                sb = kstr_asprintf(NULL, "{\"FFTch\":%d,\"ch\":[", gps.FFTch);

                for (i = 0; i < gps_chans; i++) {
                    c = &gps.ch[i];
                    int prn = -1;
                    char prn_s = 'x';
                    if (c->sat >= 0) {
                        prn_s = Sats[c->sat].prn_s[0];
                        prn = Sats[c->sat].prn;
                    }
                    sb = kstr_asprintf(sb, "%s{\"ch\":%d,\"prn_s\":\"%c\",\"prn\":%d,\"snr\":%d,\"rssi\":%d,\"gain\":%d,\"age\":\"%s\",\"old\":%d,\"hold\":%d,\"wdog\":%d"
                                           ",\"unlock\":%d,\"parity\":%d,\"alert\":%d,\"sub\":%d,\"sub_renew\":%d,\"soln\":%d,\"ACF\":%d,\"novfl\":%d,\"az\":%d,\"el\":%d}",
                                       i ? ", " : "", i, prn_s, prn, c->snr, c->snr, c->gain, c->age, c->too_old ? 1 : 0, c->hold, c->wdog,
                                       c->ca_unlocked, c->parity, c->alert, c->sub, c->sub_renew, c->has_soln, c->ACF_mode, c->novfl, c->az, c->el);

                    c->parity = 0;
                    c->has_soln = 0;
                    NextTask("gps_update4");
                }

                sb = kstr_asprintf(sb, "],\"stype\":%d", gps.soln_type);

                UMS hms(gps.StatDaySec / 60 / 60);

                unsigned r = (timer_ms() - gps.start) / 1000;
                if (r >= 3600) {
                    sb = kstr_asprintf(sb, ",\"run\":\"%d:%02d:%02d\"", r / 3600, (r / 60) % 60, r % 60);
                }
                else {
                    sb = kstr_asprintf(sb, ",\"run\":\"%d:%02d\"", (r / 60) % 60, r % 60);
                }

                sb = kstr_asprintf(sb, gps.ttff ? ",\"ttff\":\"%d:%02d\"" : ",\"ttff\":null", gps.ttff / 60, gps.ttff % 60);

                if (gps.StatDay != -1)
                    sb = kstr_asprintf(sb, ",\"gpstime\":\"%s %02d:%02d:%02.0f\"", Week[gps.StatDay], hms.u, hms.m, hms.s);
                else
                    sb = kstr_cat(sb, ",\"gpstime\":null");

                sb = kstr_asprintf(sb, gps.tLS_valid ? ",\"utc_offset\":\"%+d sec\"" : ",\"utc_offset\":null", gps.delta_tLS);

                if (gps.StatLat) {
                    // sb = kstr_asprintf(sb, ",\"lat\":\"%8.6f %c\"", gps.StatLat, gps.StatNS);
                    sb = kstr_asprintf(sb, ",\"lat\":%.6f", gps.sgnLat);
                    // sb = kstr_asprintf(sb, ",\"lon\":\"%8.6f %c\"", gps.StatLon, gps.StatEW);
                    sb = kstr_asprintf(sb, ",\"lon\":%.6f", gps.sgnLon);
                    sb = kstr_asprintf(sb, ",\"alt\":\"%1.0f m\"", gps.StatAlt);
                    sb = kstr_asprintf(sb, ",\"map\":\"<a href='http://wikimapia.org/#lang=en&lat=%8.6f&lon=%8.6f&z=18&m=b' target='_blank'>wikimapia.org</a>\"",
                                       gps.sgnLat, gps.sgnLon);
                }
                else {
                    // sb = kstr_asprintf(sb, ",\"lat\":null");
                    sb = kstr_cat(sb, ",\"lat\":0");
                }

                sb = kstr_asprintf(sb, ",\"acq\":%d,\"track\":%d,\"good\":%d,\"fixes\":%d,\"fixes_min\":%d,\"adc_clk\":%.6f,\"adc_corr\":%d,\"is_corr\":%d}",
                                   gps.acquiring ? 1 : 0, gps.tracking, gps.good, gps.fixes, gps.fixes_min, adc_clock_system() / 1e6, clk.adc_gps_clk_corrections, clk.is_corr ? 1 : 0);

                send_msg_encoded(conn, "MSG", "gps_update_cb", "%s", kstr_sp(sb));
                kstr_free(sb);
                NextTask("gps_update5");
                continue;
            }


            ////////////////////////////////
            // log
            ////////////////////////////////

            int firsttime;
            i = sscanf(cmd, "SET log_update=%d", &firsttime);
            if (i == 1) {
                int start;
                log_save_t* ls = &log_save;

                if (ls->not_shown == 0) {
                    start = firsttime ? 0 : conn->log_last_sent;
                    // if (start < ls->idx)
                    //	real_printf("ADM-%d log_update: ft=%d last=%d st/idx=%d-%d\n",
                    //		conn->self_idx, firsttime, conn->log_last_sent, start, ls->idx);
                    for (i = start; i < ls->idx; i++) {
                        send_msg(conn, SM_NO_DEBUG, "ADM log_msg_idx=%d", i);
                        send_msg_encoded(conn, "ADM", "log_msg_save", "%s", ls->arr[i]);
                    }
                    conn->log_last_sent = ls->idx;
                }
                else

                    if (ls->not_shown != conn->log_last_not_shown) {
                    send_msg(conn, SM_NO_DEBUG, "ADM log_msg_not_shown=%d", ls->not_shown);
                    start = firsttime ? 0 : MIN(N_LOG_SAVE / 2, conn->log_last_sent);
                    // if (start < ls->idx)
                    //	real_printf("ADM-%d log_update: ft=%d half=%d last=%d st/idx=%d-%d\n",
                    //		conn->self_idx, firsttime, N_LOG_SAVE/2, conn->log_last_sent, start, ls->idx);
                    for (i = start; i < ls->idx; i++) {
                        send_msg(conn, SM_NO_DEBUG, "ADM log_msg_idx=%d", i);
                        send_msg_encoded(conn, "ADM", "log_msg_save", "%s", ls->arr[i]);
                    }
                    conn->log_last_not_shown = ls->not_shown;
                }

                continue;
            }

            i = strcmp(cmd, "SET log_state");
            if (i == 0) {
                dump();
                continue;
            }

            i = strcmp(cmd, "SET log_blacklist");
            if (i == 0) {
                ip_blacklist_dump(true);
                continue;
            }

            i = strcmp(cmd, "SET log_clear_hist");
            if (i == 0) {
                TaskDump(TDUMP_CLR_HIST);
                continue;
            }


            ////////////////////////////////
            // console
            ////////////////////////////////

            buf_m = NULL;
            i = sscanf(cmd, "SET console_w2c=%512ms", &buf_m);
            if (i == 1) {
                kiwi_str_decode_inplace(buf_m);
                int slen = strlen(buf_m);
                // cprintf(conn, "CONSOLE write %d <%s>\n", slen, kiwi_str_ASCII_static(buf_m));
                if (conn->master_pty_fd > 0) {
                    sb = buf_m;
                    while (slen) {
                        int burst = MIN(slen, 32);
                        write(conn->master_pty_fd, sb, burst);
                        // cprintf(conn, "CONSOLE burst %d <%.*s>\n", burst, burst, sb);
                        sb += burst;
                        slen -= burst;
                    }
                }
                else
                    // clprintf(conn, "CONSOLE: not open for write\n");
                    kiwi_asfree(buf_m);
                continue;
            }

            u4_t ch;
            i = sscanf(cmd, "SET console_oob_key=%d", &ch);
            if (i == 1) {
                if (conn->console_child_pid && conn->oob_buf != NULL && ch <= 0xff) {
                    conn->oob_buf[conn->oob_w] = ch;
                    conn->oob_w++;
                    if (conn->oob_w == N_OOB_BUF) conn->oob_w = 0;
                    TaskWakeupF(conn->console_task_id, TWF_CANCEL_DEADLINE);
                }
                continue;
            }

            int rows, cols;
            i = sscanf(cmd, "SET console_rows_cols=%d,%d", &rows, &cols);
            if (i == 2) {
                if (conn->master_pty_fd > 0) {
                    struct winsize ws;
                    ws.ws_row = rows;
                    ws.ws_col = cols;
                    // printf("console rows=%d cols=%d\n", rows, cols);
                    scall("TIOCSWINSZ", ioctl(conn->master_pty_fd, TIOCSWINSZ, &ws));
                    // scall("TIOCGWINSZ", ioctl(conn->master_pty_fd, TIOCGWINSZ, &ws));
                    // printf("console TIOCGWINSZ %d,%d\n", ws.ws_row, ws.ws_col);
                }
                continue;
            }

            i = strcmp(cmd, "SET console_open");
            if (i == 0) {
                if (conn->console_child_pid == 0) {
                    bool no_console = false;
                    if (kiwi_file_exists(DIR_CFG "/opt.no_console"))
                        no_console = true;

                    bool console_local = admcfg_bool("console_local", NULL, CFG_REQUIRED);

                    // conn->isLocal can be forced false for testing by using the URL "nolocal" parameter
                    if (no_console == false && ((console_local && conn->isLocal) || !console_local)) {
                        if (conn->oob_buf == NULL) conn->oob_buf = (u1_t*)malloc(N_OOB_BUF);
                        conn->oob_w = conn->oob_r = 0;
                        conn->console_task_id = CreateTask(console_task, conn, ADMIN_PRIORITY);
                    }
                    else if (no_console) {
                        send_msg_encoded(conn, "ADM", "console_c2w", "CONSOLE: disabled because kiwi.config/opt.no_console file exists\n");
                    }
                    else {
                        send_msg_encoded(conn, "ADM", "console_c2w", "CONSOLE: only available to local admin connection\n");
                    }
                }
                continue;
            }


            ////////////////////////////////
            // extensions
            ////////////////////////////////

            i = strcmp(cmd, "ADM wspr_autorun_restart");
            if (i == 0) {
                wspr_autorun_restart();
                continue;
            }

            i = strcmp(cmd, "ADM ft8_autorun_restart");
            if (i == 0) {
                ft8_autorun_restart();
                continue;
            }

            // compute grid from GPS on-demand (similar to "SET admin_update")
            i = strcmp(cmd, "ADM get_gps_info");
            if (i == 0) {
                if (gps.StatLat) {
                    latLon_t loc;
                    char grid6[6 + SPACE_FOR_NULL];
                    loc.lat = gps.sgnLat;
                    loc.lon = gps.sgnLon;
                    if (latLon_to_grid6(&loc, grid6) == 0) {
                        grid6[6] = '\0';
                        send_msg_encoded(conn, "ADM", "gps_info", "{\"grid\":\"%s\"}", grid6);
                        kiwi_strncpy(wspr_c.rgrid, grid6, LEN_GRID);
                        // kiwi_strncpy(ft8_conf.rgrid, grid6, LEN_GRID);
                        // jksx FIXME need to do more when setting grid?
                    }
                }
                continue;
            }

            i = strcmp(cmd, "ADM get_ant_switch_nch");
            if (i == 0) {
                // printf("ADM get_ant_switch_nch\n");
                send_msg(conn, SM_NO_DEBUG, "ADM ant_switch_nch=%d", kiwi.ant_switch_nch);
                continue;
            }


            ////////////////////////////////
            // security
            ////////////////////////////////


            ////////////////////////////////
            // admin
            ////////////////////////////////

            i = strcmp(cmd, "SET admin_update");
            if (i == 0) {
                if (admcfg_bool("kiwisdr_com_register", NULL, CFG_REQUIRED) == false) {
                    // force switch to short sleep cycle so we get status returned sooner
                    wakeup_reg_kiwisdr_com(WAKEUP_REG_STATUS);
                }

                sb = kstr_asprintf(NULL, "{\"kiwisdr_com\":%d", reg_kiwisdr_com_status);

                if (gps.StatLat) {
                    latLon_t loc;
                    char grid6[6 + SPACE_FOR_NULL];
                    loc.lat = gps.sgnLat;
                    loc.lon = gps.sgnLon;
                    if (latLon_to_grid6(&loc, grid6))
                        grid6[0] = '\0';
                    else
                        grid6[6] = '\0';
                    sb = kstr_asprintf(sb, ",\"lat\":\"%4.2f\",\"lon\":\"%4.2f\",\"grid\":\"%s\"",
                                       gps.sgnLat, gps.sgnLon, grid6);
                }
                sb = kstr_cat(sb, "}");
                send_msg_encoded(conn, "ADM", "admin_update", "%s", kstr_sp(sb));
                kstr_free(sb);
                continue;
            }

            i = strcmp(cmd, "SET extint_load_extension_configs");
            if (i == 0) {
                extint_load_extension_configs(conn);
                continue;
            }

            i = strcmp(cmd, "SET restart");
            if (i == 0) {
                clprintf(conn, "ADMIN: restart requested by admin..\n");
                kiwi_restart();
                continue;
            }

            i = strcmp(cmd, "SET reboot");
            if (i == 0) {
                clprintf(conn, "ADMIN: reboot requested by admin..\n");
                system_reboot();
                while (true)
                    kiwi_usleep(100000);
            }

            i = strcmp(cmd, "SET power_off");
            if (i == 0) {
                clprintf(conn, "ADMIN: power off requested by admin..\n");
                system_poweroff();
                while (true)
                    kiwi_usleep(100000);
            }


            // we see these sometimes; not part of our protocol
            if (strcmp(cmd, "PING") == 0)
                continue;

            if (conn->auth != true || conn->auth_admin != true) {
                clprintf(conn, "ADMIN: cmd after auth revoked? auth=%d auth_admin=%d %s <%.64s>\n",
                         conn->auth, conn->auth_admin, conn->remote_ip, cmd);
                continue;
            }
            else {
                cprintf(conn, "ADMIN: unknown command: %s <%s>\n", conn->remote_ip, cmd);
            }
            continue;
        }

        conn->keep_alive = timer_sec() - conn->keepalive_time;
        bool keepalive_expired = (conn->keep_alive > KEEPALIVE_SEC);

        // ignore expired keepalive if disabled
        if ((admin_keepalive && keepalive_expired) || conn->kick) {
            cprintf(conn, "ADMIN connection closed\n");
            rx_server_remove(conn);
            return;
        }

        TaskSleepMsec(250);
    }
}
