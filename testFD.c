/*** 
 * @Author       : stoneBeast
 * @Date         : 2024-11-25 15:53:29
 * @Encoding     : UTF-8
 * @LastEditors  : Please set LastEditors
 * @LastEditTime : 2024-11-29 10:16:40
 * @Description  : 使用fifo模拟串口，测试程序
 */

// TODO: 解决debug时两个串口选项指定同一个串口设备的问题, 目前在使用 '-d fifo' 命令时会出现预料之外的行为导致程序崩溃

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

#define IS_DEBUG    1   /* 测试模式标志 */
#define TEST_SELF   1   /* 自测功能测试标志 */
#define DEBUG_INFO  0   /* debug输出标志 */

#define BUF_LEN 126-33+1+1
#define READ_FD 0
#define WRITE_FD 1
#define END_SIG 0x1234FAFA

#define OPTIONS "ecsd"
#define OPT_PREFIX_RET(opt,pre) ((unsigned char)((opt<<4)|(pre<<0)))
#define GET_OPT(ret)    ((unsigned char)((ret>>4)&(0x0f)))
#define GET_PREFIX(ret)    ((unsigned char)((ret)&(0x0f)))
#define OPTION_CONNECTION   'c'
#define OPTION_SELFTEST     's'
#define OPTION_EACHOTHER    'e'
#define OPTION_DEBUGCOM     'd'

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

static sem_t sem_mw_tr, sem_mr_tw;
static char* com_prefix;

/*** 
 * @brief 
 * @param argc [int]    传入参数个数
 * @param argv [char**] 传入参数数组
 * @return [int]
 */
int main(int argc, char **argv)
{
    int pipe_fd[2];                 /* 用于与子线程通信的pipe的fd */
    char test_buf[BUF_LEN] = {0};   /* 存放测试数据 */
    char write_buf[BUF_LEN] = {0};  /* 用于接收子进程回报的数据 */
    int i;                          /* 分别存放在主线程中与主线程、子线程有关的for i */
    int t_i;
    char m_fifo_name[280] = {0};    /* 主线程、子线程打开的设备的名称以及fd */
    int m_fifo_fd;
    char t_fifo_name[280] = {0};
    int t_fifo_fd;
    pthread_t thread;               /* 子线程的句柄 */
    int end = END_SIG;              /* 结束信号，由主线程在结束时发给子线程 */
    unsigned char arg_ret;          /* 接收check_args()返回的结果 */
    char option_ret;                /* 选中的选项 */
    struct dirent **comlist;        /* 存放所有符合条件的设备文件实例 */
    int com_count;                  /* comlist的长度 */
    int failed_count = 0;           /* 测试未通过的设备数量 */
    char **test_failed_list;        /* 未通过测试的设备名称列表 */
    int odd_count_flag = 0;         /* 对测，且设备个数为奇数标志 */

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
    printf("count: %d\n", com_count);

    if (option_ret == OPTION_CONNECTION)
    {
        printf("\e[1;32mconnections:\e[0m\n");
        display_connection(com_count, comlist);
        return 0;
    }

    /* 如果当前是测试选项 */
    if (option_ret == OPTION_DEBUGCOM)
    {
        if (check_com_args(&(argv[3]), comlist, com_count) == 0)
        {
            printf("error: invalid com device name\n");
            return -1;
        }

        option_ret = OPTION_EACHOTHER;
        com_count = 2;
    }

    if ((option_ret == OPTION_EACHOTHER) && (com_count%2 == 1))
    {
        odd_count_flag = 1;
        com_count --;
    }

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
            printf("open %s error\n", m_fifo_name);
            return -1;
        }

        /* 如果当前是自测，则子线程需要监听的设备fd以及设备文件名均与主线程相同 */
        if (option_ret == OPTION_SELFTEST)
        {
            strcpy(t_fifo_name, m_fifo_name);
            t_fifo_fd = m_fifo_fd;
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
            /* 打开设备，理论可以以只读打开，原因详见上方TODO */
            t_fifo_fd = open(t_fifo_name, O_RDWR);
            if (t_fifo_fd<=0)
            {
                printf("open %s error\n", t_fifo_name);
                return -1;
            }
        }
        
        /* 通过pipe将需要测试的设备fd发送给接收线程，并通过post信号量通知子线程 */
        write(pipe_fd[WRITE_FD], &t_fifo_fd, sizeof(int));
#if DEBUG_INFO
        sem_getvalue(&sem_mw_tr, &m_temp_sem_val);
        printf("%d:sem_mw_tr: %d\n", __LINE__, m_temp_sem_val);
#endif //! DEBUG_INFO
        sem_post(&sem_mw_tr);

#if IS_DEBUG==1
        if (test_i == 3)
        {
            heavy_work();
        }
        else
        {
#endif  //!IS_DEBUG==1
            /* 发送测试数据，并通过post信号量通知子线程 */
            write(m_fifo_fd, test_buf, BUF_LEN);
#if DEBUG_INFO
            sem_getvalue(&sem_mw_tr, &m_temp_sem_val);
            printf("%d:sem_mw_tr: %d\n", __LINE__, m_temp_sem_val);
#endif //! DEBUG_INFO
            // sem_post(&sem_mw_tr);
#if IS_DEBUG == 1
        }
#endif //! IS_DEBUG==1

#if DEBUG_INFO
        sem_getvalue(&sem_mr_tw, &m_temp_sem_val);
        printf("%d:sem_mr_tw: %d\n", __LINE__, m_temp_sem_val);
#endif //! DEBUG_INFO
        /* 等待子线程接收完成，并接收结果 */
        sem_wait(&sem_mr_tw);
        read(pipe_fd[READ_FD], write_buf, BUF_LEN);
        
        /* 对结果进行判断 */
        if (strcmp(write_buf, "timeout") == 0)
        {
            // failed_count++;
            add_failed_list(test_failed_list, &failed_count, t_fifo_name);
            // printf("%s send\n%s recv\n", m_fifo_name, t_fifo_name);
            printf("%s send: %s\n", m_fifo_name, test_buf);
            printf("%s recv: %s\n", t_fifo_name, write_buf);
            printf("\e[1;31m timeout error\e[0m\n");
            printf("===========================================\n");
        }
        else if(strcmp(write_buf, "error") == 0)
        {
            // failed_count++;
            add_failed_list(test_failed_list, &failed_count, t_fifo_name);
            printf("%s send\n%s recv\n", m_fifo_name, t_fifo_name);
            printf("\e[1;31m timeout error\e[0m\n");
            printf("===========================================\n");
        }
        else if(memcmp(write_buf, test_buf, BUF_LEN) != 0)
        {
            // failed_count++;
            add_failed_list(test_failed_list, &failed_count, t_fifo_name);
            printf("%s send: %s\n", m_fifo_name, test_buf);
            // printf("%s recv: %s\n", t_fifo_name, write_buf);
            printf("%s recv: ", t_fifo_name);
            diff_buf(test_buf, write_buf);
            printf("\e[1;31m error\e[0m\n");
            printf("===========================================\n");
        }
        else
        {
            // printf("%s send: %s\n", m_fifo_name, test_buf);
            // printf("%s recv: %s\n", t_fifo_name, write_buf);
            printf("%s send \e[1;32m ok \e[0m\n", m_fifo_name);
            printf("%s recv \e[1;32m ok \e[0m\n", t_fifo_name);
            printf("\e[1;32m Test Pass \e[0m\n");
            printf("===========================================\n");
        }
        /* 关闭当前设备 */
        close(m_fifo_fd);
        if (option_ret == OPTION_EACHOTHER)
        {
            close(t_fifo_fd);
        }
#if DEBUG_INFO
        printf("close fifo\n");
#endif //! DEBUG_INFO
    }

#if DEBUG_INFO
    sem_getvalue(&sem_mw_tr, &m_temp_sem_val);
    printf("%d:sem_mw_tr: %d\n", __LINE__, m_temp_sem_val);
#endif //! DEBUG_INFO
    /* 向子线程发送结束信号，并通知 */
    write(pipe_fd[WRITE_FD], &end, sizeof(int));
    sem_post(&sem_mw_tr);

    /* 等待子线程退出 */
    pthread_join(thread, NULL);

    /* 释放namelist */
    free(comlist);

    // printf("com total count: %d\n", com_count);
    printf("test passed count: \e[1;32m%d\e[0m\n", (com_count - failed_count));
    printf("test failed count: \e[1;31m%d\e[0m\t", (failed_count));

    for (i = 0; i < failed_count; i++)
    {
        printf("\e[1;31m%s\e[0m\t", test_failed_list[i]);
    }

    printf("\n");

     if (odd_count_flag)
    {
        printf("not test count: 1\t\e[1;31m%s\e[0m\n", comlist[com_count]->d_name);
    }

    /* 释放test_failed_list的所有空间 */
    free_list(failed_count, test_failed_list);

    return 0;
}

/*** 
 * @brief 接收线程任务函数
 * @param *arg [void]  子进程任务参数，这里传输的是主进程和子进程通信所使用的pipe的fd
 * @return [void* ] NULL
 */
static void *thread_task(void *arg)
{
    int *t_pipe_fd;                 /* pipe fd */
    int t_fifo_fd;                  /* fifo(模拟设备)fd */
    fd_set t_rset;                  /* select参数，用于监听读事件 */
    struct timeval t_wait_time;
    int err;
    char read_buf[BUF_LEN] = {0};

#if DEBUG_INFO
    int t_temp_sem_val;
#endif //! DEBUG_INFO

#if IS_DEBUG == 1
    int t_i = 0;
#endif //! IS_DEBUG==1

    /* 获取pipe fd */
    t_pipe_fd = arg;

    while (1)
    {
        /* 等待主线程发送传输完成的通知 */
#if DEBUG_INFO
        sem_getvalue(&sem_mw_tr, &t_temp_sem_val);
        printf("%d:sem_mw_tr: %d\n", __LINE__, t_temp_sem_val);
#endif //! DEBUG_INFO
        sem_wait(&sem_mw_tr);
        /* 读取需要接收设备的fd */
        read(t_pipe_fd[READ_FD], &t_fifo_fd, sizeof(int));
        // printf("thread: get fd: %d\n", t_fifo_fd);

        /* 判断不为结束信号 */
        if (t_fifo_fd != END_SIG)
        {
#if IS_DEBUG==1
            t_i++;
#endif //! IS_DEBUG==1
            /* 准备select所需参数，设置超时时间为200ms */
            t_wait_time.tv_sec = 0;
            t_wait_time.tv_usec = 1000*200;
            FD_ZERO(&t_rset); 
            FD_SET(t_fifo_fd, &t_rset);
            /* 监听设备 */
            err = select(t_fifo_fd+1, &t_rset, NULL, NULL, &t_wait_time);

            /* 超时，未接受到数据 */
            if (err == 0)
            {
                write(t_pipe_fd[WRITE_FD], "timeout", 8);
#if DEBUG_INFO
                sem_getvalue(&sem_mr_tw, &t_temp_sem_val);
                printf("%d:sem_mr_tw: %d\n", __LINE__, t_temp_sem_val);
#endif //! DEBUG_INFO
                sem_post(&sem_mr_tw);
            }
            /* select发生错误 */
            else if (err == -1) 
            {
                // printf("error\n");
                write(t_pipe_fd[WRITE_FD], "error", 6);
#if DEBUG_INFO
                sem_getvalue(&sem_mr_tw, &t_temp_sem_val);
                printf("%d:sem_mr_tw: %d\n", __LINE__, t_temp_sem_val);
#endif //! DEBUG_INFO
                sem_post(&sem_mr_tw);
            }
            /* 接收到数据 */
            else
            {
#if DEBUG_INFO
                sem_getvalue(&sem_mw_tr, &t_temp_sem_val);
                printf("%d:sem_mw_tr: %d\n", __LINE__, t_temp_sem_val);
#endif //! DEBUG_INFO
                // sem_wait(&sem_mw_tr);
                /* 等待发送完成通知 */
                read(t_fifo_fd, read_buf, BUF_LEN);

#if IS_DEBUG == 1
                if (t_i == 4)
                {
                    read_buf[13] = 'M';
                }
#endif //! IS_DEBUG==1

#if DEBUG_INFO
                sem_getvalue(&sem_mr_tw, &t_temp_sem_val);
                printf("%d:sem_mr_tw: %d\n", __LINE__, t_temp_sem_val);
#endif //! DEBUG_INFO
                /* 发送接收结果，并通知主线程 */
                write(t_pipe_fd[WRITE_FD], read_buf, BUF_LEN);
                sem_post(&sem_mr_tw);
            }
        }
        else
        {
#if DEBUG_INFO
            printf("end\n");
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
 * @param argc [int]    参数个数
 * @param argv [char**] 参数数组
 * @return [unsigned char] 0: 参数非法 [0:3]:prefix的位置, [4:7]:option的位置
 */
static unsigned char check_args(int argc, char **argv)
{
    unsigned char check_flag = 1;   /* 校验通过标志计数 */

    /* 校验参数个数以及参数格式 */
    if (argc != 3 && argc != 5)
    {
        check_flag = 0;
        printf("too many or less args\n");
    }
    else if ((argv[1])[0] != '-' && (argv[2])[0] != '-') 
    {
        check_flag = 0;
        printf("no options\n");
    }
    else if ((argv[1])[0] == '-' && (argv[2])[0] == '-') 
    {
        check_flag = 0;
        printf("no com-prefix\n");
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
        printf("Invain options\n");
        check_flag = 0;
    }

    if (check_flag == 0)
    {
        printf("usage: %s <options> <com-prefix> [<com1> <com2>]\n"
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
 * @param com_count [int] 串口数量   
 * @param com_name [**dirent]  串口设备文件实例数组
 * @return [void]
 */
static void display_connection(int s_com_count, struct dirent **com_name)
{
    int i;

    for (i = 0; i < s_com_count; i+=2)
    {
        if (i+1 >= s_com_count)
        {
            printf("\e[1;31m%6s\e[0m\n", com_name[i]->d_name);
        }
        else
        {
            printf("\e[1;31m%6s\e[0m <---> \e[1;31m%.6s\e[0m\n", com_name[i]->d_name, com_name[i+1]->d_name);
        }
    }
}

/*** 
 * @brief 用于scandir()中筛选符合条件的设备文件
 * @param *dir_ent [dirent]  待筛选的dirent对象指针
 * @return [int]    返回非0值该对象即被选中
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
            printf("%c", buf2[i]);
        }
        else if (i < diff_index[diff_point_p])
        {
            /* 说明当前周期还未开始 */
            // is_display_red = 0;
            printf("%c", buf2[i]);
        }
        else if (i>= diff_index[diff_point_p] && i< same_index[diff_point_p])
        {
            /* 当前在有误的周期中 */
            is_display_red = 1;
            printf("\e[1;31m%c\e[0m", buf2[i]);
        }
    }

    printf("\n");
}

/*** 
 * @brief 检查指定的两个参数串口是否存在
 * @param argv [char**]    传入的com参数的列表
 * @param comlist [struct dirent***]  out: 指向系统中所有待测的设备文件列表的指针,参数合法时会返回只含有参数的列表指针
 * @param com_count [int]    comlist的长度
 * @return [int]: 1: 参数合法; 0: 参数非法
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
 * @param list [char**]        列表
 * @param count [int*]         列表中已存在的项目个数
 * @param failed_name [char*]  需要添加的项目
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
