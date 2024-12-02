/*** 
 * @Author       : stoneBeast
 * @Date         : 2024-11-25 15:53:29
 * @Encoding     : UTF-8
 * @LastEditTime : 2024-12-02 23:34:19
 * @Description  : 使用fifo模拟串口，测试程序
 */

// TODO: 优化debug时选择同一个串口的调用流程
// TODO: 考虑log文件命名规则，处理ANSI控制字符输出到文件
// TODO: 从程序健壮性的角度考虑，线程创建失败以及线程结束失败的情况
// TODO: 参考其他程序处理传入参数的处理流程
// TODO: 可以考虑添加进度条
// BUG:  修复打印测试数据时，最后一个字符会出现异常字符
// TODO: 可以将出现错误的打印恢复出来
// TODO: 修改log文件存储逻辑
// TODO: 考虑添加-l和-h

#define _GNU_SOURCE

#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/select.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <stdarg.h>

#define IS_DEBUG    1   /* 测试模式标志 */
#define TEST_SELF   1   /* 自测功能测试标志 */
#define DEBUG_INFO  0   /* debug输出标志 */

#define BUF_LEN     126-33+1+1  /* buffer长度 */
#define READ_FD     0           /* pipe read/write fd */
#define WRITE_FD    1
#define END_SIG     "END"       /* 测试结束标志 */

#define OPTIONS                 "ecsd"                                  /* 合法的options，用于判断 */
#define OPT_PREFIX_RET(opt,pre) ((unsigned char)((opt<<4)|(pre<<0)))    /* 将options和prefix的位置信息放入一个8bit的数据中 */
#define GET_OPT(ret)            ((unsigned char)((ret>>4)&(0x0f)))      /* 获取options的位置信息 */
#define GET_PREFIX(ret)         ((unsigned char)((ret)&(0x0f)))         /* 获取prefix的位置信息 */
#define OPTION_CONNECTION       'c'                                     /* options */
#define OPTION_SELFTEST         's'
#define OPTION_EACHOTHER        'e'
#define OPTION_DEBUGCOM         'd'
#define LOG_CONSOLE             ((unsigned short)(0x00FF))              /* log输出类型: 终端/log file输出 */
#define LOG_FILE                ((unsigned short)(0xFF00))
#define LOG_CONSOLE_ASSERT(t)   ((t & LOG_CONSOLE) == LOG_CONSOLE)      /* 判断log输出类型 */
#define LOG_FILE_ASSERT(t)      ((t & LOG_FILE) == LOG_FILE)
#define COM_NUM(name)           (name[strlen(DEV_DIR)] == '/' ? name + strlen(DEV_DIR) + strlen(com_prefix) + 1     \
                                : name + strlen(com_prefix))            /* 获取传入设备名称设备编号开始的字符地址 */
#define OUT_NAME(name)          "COM",COM_NUM(name)                     /* 配合字符串模板输出 %s%s 输出COMx形式的设备名称 */

#if !IS_DEBUG //! IS_DEBUG==1
#define MODE_RS232 0x00
#define MODE_RS422 0x01
#define MODE_RS485 0x02

#define FIOBAUDRATE 0x1002
#define FIOSETOPTIONS 0x1003
#define FIOFLUSH 0x1004
#define SERIAL_MODE_SET 0x1005
#define CS8 0000060
#endif //! IS_DEBUG==1

#if IS_DEBUG == 1

#if TEST_SELF
#define DEV_DIR "./dev"
#else // TEST_SELF==1
#define DEV_DIR "./dev_l"
#endif //! TEST_SELF==1
static void heavy_work(void);

#else //! IS_DEBUG==1

#define DEV_DIR "/dev"

#endif //! IS_DEBUG==1


static void* thread_task(void *arg);
static unsigned char check_args(int argc, char **argv);
static void display_connection(int com_count, struct dirent **com_name);
static int selector(const struct dirent *dir_ent);
static void diff_buf(char *buf1, char *buf2);
static int check_com_args(char **com_args, struct dirent **comlist, int com_count);
static void add_failed_list(char **list, int *count, char *failed_name);
static void free_list(int list_len, char **list);
static void log_out(unsigned short log_type, const char *fmt, ...);
static void* read_task(void *arg);
static int try_get_result(int wait_ms);
static void base_info_store(int argc, char **argv, int com_count, struct dirent **comlist);

static sem_t sem_mw_tr, sem_mr_tw, sem_rt;  /* 主线程与子线程之间用于线程同步，子线程与读取线程之间用于轮询读取 */
static char* com_prefix;                    /* 设备前缀 */
static int log_fd;                          /* log文件fd */

/*** 
 * @brief 
 * @param argc [int]    传入参数个数
 * @param argv [char**] 传入参数数组
 * @return [int]
 */
int main(int argc, char **argv)
{
    int pipe_fd[2];                     /* 用于与子线程通信的pipe的fd */
    char test_buf[BUF_LEN] = {0};       /* 存放测试数据 */
    char write_buf[BUF_LEN] = {0};      /* 用于接收子进程回报的数据 */
    int i;                              /* 分别存放在主线程中与主线程、子线程有关的for i */
    int t_i;
    char m_fifo_name[280] = {0};        /* 主线程、子线程打开的设备的名称以及fd */
    int m_fifo_fd;
    char t_fifo_name[280] = {0};
    int t_fifo_fd;
    pthread_t thread;                   /* 子线程的句柄 */
    unsigned char arg_ret;              /* 接收check_args()返回的结果 */
    char option_ret;                    /* 选中的选项 */
    struct dirent **comlist;            /* 存放所有符合条件的设备文件实例 */
    int com_count;                      /* comlist的长度 */
    int failed_count = 0;               /* 测试未通过的设备数量 */
    char **test_failed_list;            /* 未通过测试的设备名称列表 */
    int odd_count_flag = 0;             /* 对测，且设备个数为奇数标志 */
    char pre_read_buf[BUF_LEN+5] = {0}; /* 存放预读取的数据 */

#if DEBUG_INFO
        int m_temp_sem_val;
#endif // !DEBUG_INFO

#if IS_DEBUG==1
    int test_i;
#endif //!IS_DEBUG


    /* 检查参数合法性，并获取com_prefix */
    arg_ret = check_args(argc, argv);
    if (arg_ret == 0)
    {
        return -1;
    }
    /* 获取设备文件名前缀以及选项 */
    else
    {
        com_prefix = argv[GET_PREFIX(arg_ret)];
        option_ret = argv[GET_OPT(arg_ret)][1];
    }

    /* 获取所有待测设备，并按照名称排序 */
    com_count = scandir(DEV_DIR, &comlist, selector, versionsort);
    log_out(LOG_CONSOLE, "count: %d\n", com_count);
    
    if (option_ret == OPTION_CONNECTION)
    {
        log_out(LOG_CONSOLE, "\e[1;32mconnections:\e[0m\n");
        display_connection(com_count, comlist);
        return 0;
    }

    /* 如果当前是测试选项 */
    if (option_ret == OPTION_DEBUGCOM)
    {
        if (check_com_args(&(argv[3]), comlist, com_count) == 0)
        {
            log_out(LOG_CONSOLE, "error: invalid com device name\n");
            return -1;
        }

        com_count = 2;
    }

    if ((option_ret == OPTION_EACHOTHER || option_ret == OPTION_DEBUGCOM) && (com_count%2 == 1))
    {
        odd_count_flag = 1;
        com_count --;
    }

    log_fd = open("./log.txt", O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (log_fd == -1)
    {
        log_out(LOG_CONSOLE, "create log file error\n");
    }
    base_info_store(argc, argv,(odd_count_flag? com_count+1:com_count), comlist);

    /* 申请与总设备数量相同的空间，用于存放测试失败的设备 */
    test_failed_list = malloc(sizeof(char*)*com_count);

    /* 填充测试数据，覆盖所有可视字符 */
    for (i = 0; i < BUF_LEN; i++) 
    {
        test_buf[i] = i+33;
    }

    /* 初始化两线程信息传输pipe以及同步信号量 */
    pipe(pipe_fd);
    sem_init(&sem_mr_tw, 0, 0);
    sem_init(&sem_mw_tr, 0, 0);

    /* 创建子线程，负责接收主线程从通过设备发送的数据 */
    pthread_create(&thread, NULL, thread_task, pipe_fd);

#if IS_DEBUG == 1
    test_i = 0;
#endif //! IS_DEBUG==1

    /* 循环获取目录下的所有文件对象 */
    for (i=0; i<com_count; i++)
    {
#if IS_DEBUG==1
        test_i++;
#endif //! IS_DEBUG==1
        /* 拼接设备文件完整路径 */
        sprintf(m_fifo_name, "%s/%s", DEV_DIR, comlist[i]->d_name);

        /* 清空接收buffer */
        memset(write_buf, 0, BUF_LEN);

        /* 打开设备 */
        // TODO: 由于用于模拟设备的fifo的限制，只能都以读写模式打开，实际的环境中可以测试以只写模式打开
        if (option_ret == OPTION_SELFTEST)
        {
            m_fifo_fd = open(m_fifo_name, O_RDWR);
        }
        else
        {
            m_fifo_fd = open(m_fifo_name, O_RDWR);
        }

        if (m_fifo_fd<=0)
        {
            log_out(LOG_CONSOLE, "open %s%s error\n", OUT_NAME(m_fifo_name));
            return -1;
        }

#if !IS_DEBUG //! IS_DEBUG==1
        ioctl(m_fifo_fd, FIOSETOPTIONS, CS8);
        ioctl(m_fifo_fd, FIOBAUDRATE, 115200);
        ioctl(m_fifo_fd, SERIAL_MODE_SET, MODE_RS422);
#endif //! IS_DEBUG==1

        /* 如果当前是自测，则子线程需要监听的设备fd以及设备文件名均与主线程相同 */
        if (option_ret == OPTION_SELFTEST)
        {
            strcpy(t_fifo_name, m_fifo_name);
        }
        else    /* option_ret == OPTION_EACHOTHER */
        {
            /* 如果i为0或2的倍数，则说明当前进行的是一组对测的第一次测试 */
            if (i%2 == 0)
            {
                t_i = i+1;
            }
            else
            {
                t_i = i-1;
            }

            /* 获取子线程需要监听的设备的文件名 */
            sprintf(t_fifo_name, "%s/%s", DEV_DIR, comlist[t_i]->d_name);
        }

        t_fifo_fd = open(t_fifo_name, O_RDWR);
#if !IS_DEBUG //! IS_DEBUG==1
        ioctl(t_fifo_fd, FIOSETOPTIONS, CS8);
        ioctl(t_fifo_fd, FIOBAUDRATE, 115200);
        ioctl(t_fifo_fd, SERIAL_MODE_SET, MODE_RS422);
#endif //! IS_DEBUG==1
        write(t_fifo_fd, "test", 5);
        usleep(10 * 1000);
        read(t_fifo_fd, pre_read_buf, BUF_LEN+5);
        close(t_fifo_fd);

        /* 通过pipe将需要测试的设备fd发送给接收线程，并通过post信号量通知子线程 */
        write(pipe_fd[WRITE_FD], t_fifo_name, (strlen(t_fifo_name)+1));
#if DEBUG_INFO
        sem_getvalue(&sem_mw_tr, &m_temp_sem_val);
        log_out(LOG_CONSOLE, "%d: m: send fifo fd: sem_mw_tr: %d\n", __LINE__, m_temp_sem_val);
#endif //! DEBUG_INFO
        sem_post(&sem_mw_tr);

#if IS_DEBUG==1
        if (test_i == 3)
        {
            /* do nothing */
        }
        else
        {
#endif  //!IS_DEBUG==1
            /* 发送测试数据，并通过post信号量通知子线程 */
            write(m_fifo_fd, test_buf, BUF_LEN);
#if DEBUG_INFO
            sem_getvalue(&sem_mw_tr, &m_temp_sem_val);
            log_out(LOG_CONSOLE, "%d: m: after send buf: sem_mw_tr: %d\n", __LINE__, m_temp_sem_val);
#endif //! DEBUG_INFO

#if IS_DEBUG == 1
        }
#endif //! IS_DEBUG==1

#if DEBUG_INFO
        sem_getvalue(&sem_mr_tw, &m_temp_sem_val);
        log_out(LOG_CONSOLE, "%d: m: wait response: sem_mr_tw: %d\n", __LINE__, m_temp_sem_val);
#endif //! DEBUG_INFO
        /* 等待子线程接收完成，并接收结果 */
        sem_wait(&sem_mr_tw);
        read(pipe_fd[READ_FD], write_buf, BUF_LEN);
        
        /* 对结果进行判断 */
        if (strcmp(write_buf, "timeout") == 0)
        {
            add_failed_list(test_failed_list, &failed_count, t_fifo_name);
            log_out(LOG_FILE, "%s%s send: %s\n", OUT_NAME(m_fifo_name), test_buf);
            log_out(LOG_FILE, "%s%s recv: %s\n", OUT_NAME(t_fifo_name), write_buf);
            log_out(LOG_FILE, "\e[1;31m timeout error\e[0m\n");
            log_out(LOG_FILE, "===========================================\n");
        }
        else if(strcmp(write_buf, "error") == 0)
        {
            add_failed_list(test_failed_list, &failed_count, t_fifo_name);
            log_out(LOG_FILE, "%s%s send\n%s%s recv\n", OUT_NAME(m_fifo_name), OUT_NAME(t_fifo_name));
            log_out(LOG_FILE, "\e[1;31m timeout error\e[0m\n");
            log_out(LOG_FILE, "===========================================\n");
        }
        else if(memcmp(write_buf, test_buf, BUF_LEN) != 0)
        {
            add_failed_list(test_failed_list, &failed_count, t_fifo_name);
            log_out(LOG_FILE, "%s%s send: %s\n", OUT_NAME(m_fifo_name), test_buf);
            log_out(LOG_FILE, "%s%s recv: ", OUT_NAME(t_fifo_name));
            diff_buf(test_buf, write_buf);
            log_out(LOG_FILE, "\e[1;31m error\e[0m\n");
            log_out(LOG_FILE, "===========================================\n");
        }
        else
        {
            log_out(LOG_FILE, "%s%s send: %s\n", OUT_NAME(m_fifo_name), test_buf);
            log_out(LOG_FILE, "%s%s recv: %s\n", OUT_NAME(t_fifo_name), write_buf);
            log_out(LOG_FILE, "%s%s send \e[1;32m ok \e[0m\n", OUT_NAME(m_fifo_name));
            log_out(LOG_FILE, "%s%s recv \e[1;32m ok \e[0m\n", OUT_NAME(t_fifo_name));
            log_out(LOG_FILE, "\e[1;32m Test Pass \e[0m\n");
            log_out(LOG_FILE, "===========================================\n");
        }

        /* 关闭当前设备 */
        close(m_fifo_fd);

#if DEBUG_INFO
        log_out(LOG_CONSOLE, "close fifo\n");
#endif //! DEBUG_INFO
    }

#if DEBUG_INFO
    sem_getvalue(&sem_mw_tr, &m_temp_sem_val);
    log_out(LOG_CONSOLE, "%d: m: send end: sem_mw_tr: %d\n", __LINE__, m_temp_sem_val);
#endif //! DEBUG_INFO
    /* 向子线程发送结束信号，并通知 */
    write(pipe_fd[WRITE_FD], END_SIG, strlen(END_SIG)+1);
    sem_post(&sem_mw_tr);

    /* 等待子线程退出 */
    pthread_join(thread, NULL);

    /* 释放namelist */
    free(comlist);

    log_out(LOG_FILE|LOG_CONSOLE, "test passed count: \e[1;32m%d\e[0m\n", (com_count - failed_count));
    log_out(LOG_FILE|LOG_CONSOLE, "test failed count: \e[1;31m%d\e[0m\t", (failed_count));

    for (i = 0; i < failed_count; i++)
    {
        log_out(LOG_FILE | LOG_CONSOLE, "\e[1;31m%s%s\e[0m\t", OUT_NAME(test_failed_list[i]));
    }

    log_out(LOG_FILE|LOG_CONSOLE, "\n");

     if (odd_count_flag)
    {
        log_out(LOG_FILE|LOG_CONSOLE, "not test count: 1\t\e[1;31m%s%s\e[0m\n", OUT_NAME(comlist[com_count]->d_name));
    }

    /* 释放test_failed_list的所有空间 */
    free_list(failed_count, test_failed_list);

    return 0;
}

/*** 
 * @brief 接收线程任务函数
 * @param *arg [void]   子进程任务参数，这里传输的是主进程和子进程通信所使用的pipe的fd
 * @return [void* ]     NULL
 */
static void *thread_task(void *arg)
{
    int *t_pipe_fd;                 /* pipe fd */
    int t_fifo_fd;                  /* fifo(模拟设备)fd */
    char t_fifo_name[280] = {0};    /* fifo(模拟设备)设备文件名 */
    char read_buf[BUF_LEN] = {0};   /* 存放读取到的数据 */
    pthread_t r_thread;             /* 读取线程句柄 */
    void *temp_res;                 /* 线程cancel返回结果 */
    int read_flag;                  /* 保存 try_get_result() 返回的结果 */

#if DEBUG_INFO
    int t_temp_sem_val;
#endif //! DEBUG_INFO

#if IS_DEBUG == 1
    int t_i = 0;
#endif //! IS_DEBUG==1

    /* 获取pipe fd */
    t_pipe_fd = arg;

    sem_init(&sem_rt, 0, 0);

    while (1)
    {
        /* 开始前线先清空buffer */
        memset(read_buf, 0, BUF_LEN);
        /* 等待主线程发送传输完成的通知 */
#if DEBUG_INFO
        sem_getvalue(&sem_mw_tr, &t_temp_sem_val);
        log_out(LOG_CONSOLE, "%d: t: wait fifo fd: sem_mw_tr: %d\n", __LINE__, t_temp_sem_val);
#endif //! DEBUG_INFO
        sem_wait(&sem_mw_tr);
        /* 读取需要接收设备的fd */
        read(t_pipe_fd[READ_FD], t_fifo_name, 280);

        /* 判断不为结束信号 */
        if (strncmp(END_SIG, t_fifo_name, strlen(END_SIG)) != 0 )
        {
#if IS_DEBUG==1
            t_i++;
#endif //! IS_DEBUG==1
            t_fifo_fd = open(t_fifo_name, O_RDWR);
            // t_fifo_fd = open(t_fifo_name, O_RDWR|O_NONBLOCK);
            if (t_fifo_fd<=0)
            {
                log_out(LOG_CONSOLE, "open %s%s error\n", OUT_NAME(t_fifo_name));
                pthread_exit(&t_fifo_fd);
            }

#if !IS_DEBUG //! IS_DEBUG==1
            ioctl(t_fifo_fd, FIOSETOPTIONS, CS8);
            ioctl(t_fifo_fd, FIOBAUDRATE, 115200);
            ioctl(t_fifo_fd, SERIAL_MODE_SET, MODE_RS422);
#endif //! IS_DEBUG==1

            pthread_create(&r_thread, NULL, read_task, &t_fifo_fd);
            read_flag = try_get_result(100);
            if (0 == read_flag)
            {
                pthread_cancel(r_thread);
            }
            
            pthread_join(r_thread, &temp_res);
            if (0 == read_flag)
            {
                if (temp_res == PTHREAD_CANCELED)
                {
#if DEBUG_INFO
                    log_out(LOG_CONSOLE, "cancel thread success\n");
#endif //! DEBUG_INFO

                    write(t_pipe_fd[WRITE_FD], "timeout", 8);
#if DEBUG_INFO
                    sem_getvalue(&sem_mr_tw, &t_temp_sem_val);
                    log_out(LOG_CONSOLE,
                            "%d: t: send response: sem_mr_tw: %d\n", __LINE__,
                            t_temp_sem_val);
#endif //! DEBUG_INFO
                    sem_post(&sem_mr_tw);
                }
                else
                {
                    log_out(LOG_CONSOLE, "cancel thread failed\n");
                }
            }
            else
            {
                strcpy(read_buf, temp_res);
                free(temp_res);
#if DEBUG_INFO
                log_out(LOG_CONSOLE, "res: %s\n", read_buf);
                sem_getvalue(&sem_mw_tr, &t_temp_sem_val);
                log_out(LOG_CONSOLE, "%d: t: before read buf: sem_mw_tr: %d\n",
                        __LINE__, t_temp_sem_val);
#endif //! DEBUG_INFO

#if IS_DEBUG == 1
                if (t_i == 4) {
                  read_buf[13] = 'M';
                }
#endif //! IS_DEBUG==1

#if DEBUG_INFO
                sem_getvalue(&sem_mr_tw, &t_temp_sem_val);
                log_out(LOG_CONSOLE,
                        "%d: t: before send response: sem_mr_tw: %d\n",
                        __LINE__, t_temp_sem_val);
#endif //! DEBUG_INFO
                /* 发送接收结果，并通知主线程 */
                write(t_pipe_fd[WRITE_FD], read_buf, BUF_LEN);
#if DEBUG_INFO
                log_out(LOG_CONSOLE, "thread recv: %s\n", read_buf);
#endif //! DEBUG_INFO
                sem_post(&sem_mr_tw);

                close(t_fifo_fd);
            }
        }
        else
        {
#if DEBUG_INFO
            log_out(LOG_CONSOLE, "end\n");
#endif //! DEBUG_INFO
            return 0;
        }
    }
    return 0;
}

#if IS_DEBUG == 1
static void heavy_work(void)
{
    long i = 0;
    while (i<1000000000)
    {
        i++;
    }

    return;
}
#endif //! IS_DEBUG==1

/*** 
 * @brief   判断参数合法性，并返回options的索引；强制要求前两个参数一个是选项，一个是前缀
 * @param argc [int]        参数个数
 * @param argv [char**]     参数数组
 * @return [unsigned char]  0: 参数非法 [0:3]:prefix的位置, [4:7]:option的位置
 */
static unsigned char check_args(int argc, char **argv)
{
    unsigned char check_flag = 1;   /* 校验通过标志计数 */

    /* 校验参数个数以及参数格式 */
    if (argc != 3 && argc != 5)
    {
        check_flag = 0;
        log_out(LOG_CONSOLE, "\e[1;31mToo many or less args\e[0m\n");
    }
    else if ((argv[1])[0] != '-' && (argv[2])[0] != '-') 
    {
        check_flag = 0;
        log_out(LOG_CONSOLE, "\e[1;31mNo options\e[0m\n");
    }
    else if ((argv[1])[0] == '-' && (argv[2])[0] == '-') 
    {
        check_flag = 0;
        log_out(LOG_CONSOLE, "\e[1;31mNo com-prefix\e[0m\n");
    }
    else if ((argv[1])[0] == '-')   /* 获取选项参数以及前缀参数的位置 */
    {
        check_flag = OPT_PREFIX_RET(1, 2);
    }
    else
    {
        check_flag = OPT_PREFIX_RET(2, 1);
    }

    /* 校验选项参数是否合法 */
    if(NULL == strchr(OPTIONS, (argv[GET_OPT(check_flag)])[1]))
    {
        log_out(LOG_CONSOLE, "\e[1;31mInvain options\e[0m\n");
        check_flag = 0;
    }

    /* 检查某些特定组合 */
    if (argc != 5 && ((argv[GET_OPT(check_flag)])[1] == OPTION_DEBUGCOM))
    {
        check_flag = 0;
        log_out(LOG_CONSOLE, "\e[1;31m-d Must have 2 COM args\e[0m\n");
    }

    if (check_flag == 0)
    {
        log_out(LOG_CONSOLE, "usage: %s <options> <com-prefix> [<com1> <com2>]\n"
                            "\toptions:\n"
                                "\t\t-e: one transmit one receive\n"
                                "\t\t-s: self transmit and receive\n"
                                "\t\t-c: display connections\n"
                                "\t\t-d: -d <com-prefix> <com1> <com2> :debug com1 and com2\n"
                            "\tcom-prefix: \n"
                                "\t\tcom device name prefix\n", argv[0]);
    }

    return check_flag;
}

/*** 
 * @brief 打印串口连接关系
 * @param com_count [int]       串口数量   
 * @param com_name [**dirent]   串口设备文件实例数组
 * @return [void]
 */
static void display_connection(int s_com_count, struct dirent **com_name)
{
    int i;

    for (i = 0; i < s_com_count; i+=2)
    {
        if (i+1 >= s_com_count)
        {
            log_out(LOG_CONSOLE, "\e[1;31m%s%s\e[0m\n", OUT_NAME(com_name[i]->d_name));
        }
        else
        {
          log_out(LOG_CONSOLE, "\e[1;31m%s%s\e[0m <---> \e[1;31m%s%s\e[0m\n",
                            OUT_NAME(com_name[i]->d_name),
                            OUT_NAME(com_name[i + 1]->d_name));
        }
    }
}

/*** 
 * @brief 用于scandir()中筛选符合条件的设备文件
 * @param *dir_ent [dirent]     待筛选的dirent对象指针
 * @return [int]                返回非0值该对象即被选中
 */
static int selector(const struct dirent *dir_ent)
{
    if (strncmp(com_prefix, dir_ent->d_name, strlen(com_prefix)) == 0)
    {
        return 1;
    }

    return 0;
}

/*** 
 * @brief 对比buf1和buf2,将buf2与buf1不同的部分用不同的颜色显示出来(未经过充分测试)
 * @param *buf1 [char]    参照字符串
 * @param *buf2 [char]    对比字符串
 * @return [void]
 */
static void diff_buf(char *buf1, char *buf2)
{
    /* 
        本函数中，将以第一个不同的字符出现的索引作为一个周期的开始，直到出现第一个相
        同的字符作为该周期的结束周期之间的字符全部以特殊颜色显示出来(不包括第一个相同的字符)
     */

    int diff_index[10];                 /* 记录周期开始、结束的节点 */
    int same_index[10];
    int diff_point = 0;                 /* 记录节点的个数 */
    int same_point = 0;
    int buf1_len, buf2_len, len;        /* 记录两个字符串的长度 */
    int i;
    int diff_point_p = 0;               /* 当前处于的周期，从1开始计数 */
    int is_display_red = 0;
    int flag = 0;                       /* flah=1, 找相同的点, 0找不同的点 */

    memset(diff_index, -1, 10);
    memset(same_index, -1, 10);

    buf1_len = strlen(buf1);
    buf2_len = strlen(buf2);

    /* 获取两字符串的最短长度作为遍历长度 */
    if (buf1_len < buf2_len)
    {
        len = buf1_len;
    }
    else
    {
        len = buf2_len;
    }

    for (i = 0; i < len; i++)
    {
        if ((buf1[i]==buf2[i]) == flag)
        {
            /* 找到当前需要找到的point, 记录到对应的数组中 */
            if (flag)
            {
                same_index[same_point++] = i;
                flag = 0;
            }
            else
            {
                diff_index[diff_point++] = i;
                flag = 1;
            }
        }
    }

    /* 如果对比字符串比参照字符串长，当查找结束后仍在寻找新的周期的开始，则以参照字符串长度的索引为开始，作为新周期的开始 */
    if ((buf2_len > buf1_len) && (flag==0))
    {
        diff_index[diff_point++] = i;
        flag = 1;
    }

    /* 如果直到循环结束都在寻找当前周期的结束，则将最后一个字符的索引作为结束 */
    if (flag == 1)
    {
        same_index[same_point++] = buf2_len;
    }

    /* 打印比较完成后的字符串 */
    for (size_t i = 0; i < buf2_len; i++)
    {
        if (i>=same_index[diff_point_p])
        {
            /* 当前已经结束上一个周期 */
            diff_point_p++;
            is_display_red = 0;
            log_out(LOG_FILE, "%c", buf2[i]);
        }
        else if (i < diff_index[diff_point_p])
        {
            /* 说明当前周期还未开始 */
            // is_display_red = 0;
            log_out(LOG_FILE, "%c", buf2[i]);
        }
        else if (i>= diff_index[diff_point_p] && i< same_index[diff_point_p])
        {
            /* 当前在有误的周期中 */
            is_display_red = 1;
            log_out(LOG_FILE, "\e[1;31m%c\e[0m", buf2[i]);
        }
    }

    log_out(LOG_FILE, "\n");
}

/*** 
 * @brief 检查指定的两个参数串口是否存在
 * @param argv [char**]                 传入的com参数的列表
 * @param comlist [struct dirent***]    out: 指向系统中所有待测的设备文件列表的指针,参数合法时会返回只含有参数的列表指针
 * @param com_count [int]               comlist的长度
 * @return [int]:                       1: 参数合法; 0: 参数非法
 */
static int check_com_args(char **com_args, struct dirent **s_comlist, int s_com_count)
{
    int check_flag = 0;         /* 检查通过标志计数 */
    int i, com1_inx, com2_inx;  /* com1_inx,com2_inx: 记录两个参数在comlist中匹配的索引 */
    struct dirent temp1;        /* 临时变量 */
    struct dirent temp2;

    /* 循环比较列表中的成员是否包含传入的参数 */
    for (i = 0; i < s_com_count; i++)
    {
        if (strcmp(com_args[0], (s_comlist)[i]->d_name) == 0) 
        {
            /* 记录索引，递增标志计数 */
            com1_inx = i;
            check_flag++;
        }
        /* 允许两个测试串口选择同一个 */
        // if (strcmp(com_args[1], (s_comlist)[i]->d_name) == 0)
        else if (strcmp(com_args[1], (s_comlist)[i]->d_name) == 0)
        {
            com2_inx = i;
            check_flag++;
        }

        if (check_flag == 2)
        {
            break;
        }
    }

    if (check_flag == 2)
    {
        /* 两个串口都存在 */
        /* 将两个设备文件信息保存出来 */
        memcpy(&temp1, (s_comlist)[com1_inx], sizeof(struct dirent));
        memcpy(&temp2, (s_comlist)[com2_inx], sizeof(struct dirent));

        strcpy((*(s_comlist[0])).d_name, temp1.d_name);
        strcpy((*(s_comlist[1])).d_name, temp2.d_name);

        return 1;
    }
    else
    {
        /* 有串口不存在 */
        return 0;
    }
}

/*** 
 * @brief 添加项目到列表
 * @param list [char**]         列表
 * @param count [int*]          列表中已存在的项目个数
 * @param failed_name [char*]   需要添加的项目
 * @return [void]
 */
static void add_failed_list(char **list, int *count, char *failed_name)
{
    list[*count] = malloc(sizeof(char)*(strlen(failed_name)+1));
    strcpy(list[*count], failed_name);
    (*count)++;
}

/***
 * @brief 释放字符串列表空间
 * @param list_len [int]    列表长度
 * @param list [char**] 需要释放的列表
 * @return [void]
 */
static void free_list(int list_len, char **list)
{
    int i;

    /* 循环释放列表项目 */
    for (i = 0; i < list_len; i++)
    {
        free(list[i]);
    }

    /* 释放列表自身 */
    free(list);
}

/***
 * @brief 统一log输出管理
 * @param log_type [unsigned short]  从以下值中选择1-2个: LOG_FILE;LOG_CONSOLE;分别控制log输出到console和log文件
 * @param *fmt [char]               字符串模板, 用法类似printf
 * @return [void]
 */
static void log_out(unsigned short log_type, const char *fmt, ...)
{
    int i;
    int file_i = 0;
    char temp_log[512] = {0};   /* 临时存放处理之后的字符串 */
    char file_log[512] = {0};
    va_list args;               /* 参数列表 */

    va_start(args, fmt);
    vsprintf(temp_log, fmt, args);
    va_end(args);

    /* 是否输出到console */
    if (LOG_CONSOLE_ASSERT(log_type))
    {
        printf("%s", temp_log);
    }
    /* 是否输出到log文件 */
    if (LOG_FILE_ASSERT(log_type) && (log_fd != -1))
    {
        for (i = 0; i < strlen(temp_log); i++)
        {
            if (temp_log[i] == '\e')
            {
                i++;
                while (temp_log[i] != 'm') 
                {
                    i++;
                }
                if (temp_log[i] == 'm')
                {
                    i++;
                }
            }
            if (temp_log[i] != '\e')
            {
                file_log[file_i++] = temp_log[i];
            }
        }
        write(log_fd, file_log, strlen(file_log));
    }
}

/*** 
 * @brief 实际执行读取行为的线程函数
 * @param arg [void*]   线程函数参数，这里用来传输需要读取的设备的fd
 * @return [void*]      用来返回读取到的设备或是线程退出结果
 */
static void *read_task(void *arg)
{
    int r_fd;           /* 读取设备的fd */
    char *temp_buf;     /* 存放读取数据 */
    int read_len = 0;   /* 读取数据的长度 */

    /* 获取fd */
    r_fd = (*(int *)arg);
    /* 申请空间，需要在thread_task()中释放 */
    temp_buf = (char*)malloc((sizeof(char)*BUF_LEN) + 1);
    memset(temp_buf, 0, (sizeof(char) * BUF_LEN) + 1);

    /* 尝试读取数据 */
    read_len = read(r_fd, temp_buf, BUF_LEN+1);
    if(read_len < BUF_LEN-1) /* 由于读取速度相较于收发速度快很多，有可能会出现刚接收一部分就返回，导致数据不完整的可能 */
    {
        /* 休眠50ms，保证数据完全接收后，再次读取 */
        usleep(50*1000);
        read(r_fd, temp_buf+read_len, BUF_LEN-read_len);
    }
    /* 给task thread发信号 */
    sem_post(&sem_rt);

    /* 返回接收数据 */
    return temp_buf;
}

/*** 
 * @brief 尝试获取读取结果
 * @param wait_ms [int] 接收最大时长(ms)
 * @return [int]        返回结果: 0:失败 1: 成功
 */
static int try_get_result(int wait_ms)
{
    int try_times = 0;  /* 尝试获取结果的次数 */
    while (try_times<wait_ms) 
    {
        /* 尝试获取信号量 */
        if (0 == sem_trywait(&sem_rt))
        {
            break;
        }
        /* 每次尝试之间间隔1ms */
        usleep(1000*1);
        try_times++;
    }

    /* 尝试次数小于设定最大次数 */
    if (try_times < wait_ms)
    {
        /* 返回成功 */
        return 1;
    }

    /* 获取失败 */
    return 0;
}

/*** 
 * @brief 存储当前测试基本环境
 * @param argc [int]                argv项目数量
 * @param argv [char**]             参数数组
 * @param com_count [int]           测试的串口数量
 * @param comlist [struct dirent**] 测试的串口设备文件实例数组   
 * @return [void]
 */
static void base_info_store(int argc, char **argv, int com_count, struct dirent **comlist)
{
    int i;

    /* 记录启动测试的命令 */
    log_out(LOG_FILE, "command: ");
    for (i = 0; i< argc; i++)
    {
        log_out(LOG_FILE, "%s ", argv[i]);
    }
    log_out(LOG_FILE, " \n");

    /* 记录参与测试的串口数量 */
    log_out(LOG_FILE, "device count: %d\n", com_count);

    /* 罗列串口 */
    log_out(LOG_FILE, "device list: ");
    for (i = 0; i < com_count; i++)
    {
        log_out(LOG_FILE, "%s%s ", OUT_NAME(comlist[i]->d_name));
    }
    log_out(LOG_FILE, " \n\n");
}
