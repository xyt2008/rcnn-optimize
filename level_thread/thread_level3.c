#include "common.h"
#include"cblas.h"

static thread_status_t thread_status[MAX_CPU_NUMBER] __attribute__((aligned(128)));
static pthread_t     blas_threads [MAX_CPU_NUMBER];
void   sub_pthread_exec(void);

blas_queue_t  queue[MAX_CPU_NUMBER];
BLASLONG      range_M[MAX_CPU_NUMBER + 1];
BLASLONG      range_N[MAX_CPU_NUMBER + 1];
job_t         job[MAX_CPU_NUMBER];
blas_arg_t    execute_arg;


static void pthread_bind(int cpu)
{
    cpu_set_t mask; 
    CPU_ZERO(&mask);      
    CPU_SET(cpu, &mask);  
    if(sched_setaffinity(0, sizeof(mask), &mask) == -1)  
    {  
        printf("set affinity failed..");  
    }
}

//about instance matrix compute:
typedef int (*ROUTINE)(BLASLONG);
static inline int inner_thread(BLASLONG mypos)
{
    BLASLONG m_from, m_to, n_from, n_to, N_from, N_to;
    BLASLONG lda, ldb, ldc;
    BLASLONG ls, min_l, jjs, min_jj;
    BLASLONG is, min_i, div_n;
    BLASLONG i, current;

    FLOAT *a, *b, *c, *alpha, *beta;
    BLASLONG m, n, k;
    
    FLOAT ALP = 1;
    FLOAT BET = 0;

    FLOAT *sa     = queue[mypos].sa;
    FLOAT *buffer = queue[mypos].sb;
    
    a = execute_arg.a;
    b = execute_arg.b;
    c = execute_arg.c;
    m = execute_arg.m;
    n = execute_arg.n;
    k = execute_arg.k;
    lda = m;
    ldb = k;
    ldc = m;
    alpha = &ALP;
    beta  = &BET;
    
    m_from = range_M[mypos + 0];
    m_to   = range_M[mypos + 1];

    n_from = range_N[mypos + 0];
    n_to   = range_N[mypos + 1];

    N_from = range_N[0];
    N_to   = range_N[MAX_CPU_NUMBER];

    if (beta[0] == 0) BETA_OPERATION(m_from, m_to, N_from, N_to, beta, c, ldc);
  
    for(ls = 0; ls < k; ls += min_l)
    {
        min_l = k - ls;
        if      (min_l >= GEMM_Q * 2)   min_l = GEMM_Q;
        else if (min_l > GEMM_Q)        min_l = (min_l + 1) / 2;

        min_i = m_to - m_from;
        if      (min_i >= GEMM_P * 2)   min_i = GEMM_P;
        else if (min_i > GEMM_P)        min_i = ((min_i / 2 + GEMM_UNROLL_M - 1)/GEMM_UNROLL_M) * GEMM_UNROLL_M;
 
        ICOPY_OPERATION(min_l, min_i, a, lda, ls, m_from, sa);

        /* Make sure if no one is using buffer */
        for (i = 0; i < MAX_CPU_NUMBER; i++) while(job[mypos].working[i][CACHE_LINE_SIZE]) {YIELDING;};
        for(jjs = n_from; jjs < n_to; jjs += min_jj)
        {
            min_jj = n_to - jjs;
            if      (min_jj >= 3*GEMM_UNROLL_N)  min_jj = 3*GEMM_UNROLL_N;
            else if (min_jj >= 2*GEMM_UNROLL_N)  min_jj = 2*GEMM_UNROLL_N;
            else if (min_jj > GEMM_UNROLL_N)     min_jj = GEMM_UNROLL_N;

            OCOPY_OPERATION(min_l, min_jj, b, ldb, ls, jjs,
                buffer + min_l * (jjs - n_from));

            KERNEL_OPERATION(min_i, min_jj, min_l, alpha, sa,
                buffer + min_l * (jjs - n_from), c, ldc, m_from, jjs);
        }
        for (i = 0; i < MAX_CPU_NUMBER; i++) job[mypos].working[i][CACHE_LINE_SIZE] = (BLASLONG)buffer;
        WMB;

        current = mypos;
        do
        {
            current++; if (current >= MAX_CPU_NUMBER) current = 0;
            if (current != mypos)
            {
              /* thread has to wait */
              while(job[current].working[mypos][CACHE_LINE_SIZE] == 0) {YIELDING;};

              KERNEL_OPERATION(min_i, range_N[current + 1]  - range_N[current], min_l, alpha,
                       sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE],
                       c, ldc, m_from, range_N[current]);
            }
            if (m_to - m_from == min_i) job[current].working[mypos][CACHE_LINE_SIZE] &= 0;
        } while (current != mypos);

        for(is = m_from + min_i; is < m_to; is += min_i)
        {
            min_i = m_to - is;

            if      (min_i >= GEMM_P * 2)   min_i = GEMM_P;
            else if (min_i > GEMM_P)        min_i = (((min_i + 1) / 2 + GEMM_UNROLL_M - 1)/GEMM_UNROLL_M) * GEMM_UNROLL_M;

            ICOPY_OPERATION(min_l, min_i, a, lda, ls, is, sa);

            current = mypos;
            do
            {
                KERNEL_OPERATION(min_i, range_N[current + 1] - range_N[current], min_l, alpha,
                    sa, (FLOAT *)job[current].working[mypos][CACHE_LINE_SIZE], c, ldc, is, range_N[current]);

                if (is + min_i >= m_to)
                {
                    /* Thread doesn't need this buffer any more */
                    job[current].working[mypos][CACHE_LINE_SIZE] &= 0;
                    WMB;
                }

                current ++; if (current >= MAX_CPU_NUMBER) current = 0;
            } while (current != mypos);
        }
    }
    for (i = 0; i < MAX_CPU_NUMBER; i++)  while (job[mypos].working[i][CACHE_LINE_SIZE] ) {YIELDING;};
}

void divide(BLASLONG M, BLASLONG* range_M)
{
    int dx = M%MAX_CPU_NUMBER;
    int dy = M/MAX_CPU_NUMBER;
    int index = 0;
    int i;
    for(i = 0;i < MAX_CPU_NUMBER + 1; i++)
    {
        range_M[i] = index;
        if(i < dx)
        {
            index = index + dy + 1;
        }
        else
        {
            index = index + dy;
        }
    }
}

void sgemm_thread_nn(float* A, float* B, float* C, BLASLONG M, BLASLONG N, BLASLONG K)
{
    int i;
    execute_arg.a        = B;
    execute_arg.b        = A;
    execute_arg.c        = C;
    execute_arg.m        = N;
    execute_arg.n        = M;
    execute_arg.k        = K;

    divide(execute_arg.m, range_M);
    divide(execute_arg.n, range_N);

    sub_pthread_exec();
    
    ((ROUTINE)(queue[0].routine))(0);
    queue[0].assigned = 0;

    for (i = 0; i < MAX_CPU_NUMBER; i++) while (queue[i].assigned) {YIELDING;};
}
//end about instance matrix compute:

//about pthread:
void sub_pthread_exec(void)
{
    int pthread_pos;    
    for(pthread_pos = 1; pthread_pos < MAX_CPU_NUMBER; pthread_pos++)
    {
        if (thread_status[pthread_pos].status == THREAD_STATUS_SLEEP) 
        {
            pthread_mutex_lock  (&thread_status[pthread_pos].lock);
            thread_status[pthread_pos].status = THREAD_STATUS_WAKEUP;
            pthread_cond_signal(&thread_status[pthread_pos].wakeup);
            pthread_mutex_unlock(&thread_status[pthread_pos].lock);
        }
    }
}

static void* sub_pthread_body(void *arg)
{
    int  pthread_pos = (int)arg;
    pthread_mutex_lock  (&thread_status[pthread_pos].lock);

    while (thread_status[pthread_pos].status == THREAD_STATUS_SLEEP)
    {
        pthread_cond_wait(&thread_status[pthread_pos].wakeup, &thread_status[pthread_pos].lock);
    }
    pthread_mutex_unlock(&thread_status[pthread_pos].lock);

    ((ROUTINE)(queue[pthread_pos].routine))(pthread_pos);
    queue[pthread_pos].assigned = 0;
    thread_status[pthread_pos].status = THREAD_STATUS_SLEEP;
}

void sub_pthread_init(void)
{
    int i, j, pthread_pos;
    
    for (i = 0; i < MAX_CPU_NUMBER; i++)
    {
        queue[i].sa       = mmap(NULL, BUFFER_SIZE, MMAP_ACCESS, MMAP_POLICY, -1, 0);
        queue[i].sb       = (void *)(((BLASLONG)(queue[i].sa) + ((SGEMM_P * SGEMM_Q * sizeof(float) + GEMM_ALIGN) & ~GEMM_ALIGN)));
        queue[i].assigned = i + 1;
        queue[i].routine  = inner_thread;
        
        for (j = 0; j < MAX_CPU_NUMBER; j++)
        {
            job[i].working[j][CACHE_LINE_SIZE] = 0;
        }
    }

    for(pthread_pos = 1; pthread_pos < MAX_CPU_NUMBER; pthread_pos++)
    {
        pthread_mutex_init(&thread_status[pthread_pos].lock, NULL);
        pthread_cond_init (&thread_status[pthread_pos].wakeup, NULL);
        thread_status[pthread_pos].status = THREAD_STATUS_SLEEP;
        pthread_create(&blas_threads[pthread_pos], NULL, &sub_pthread_body, (void *)pthread_pos);
    }
}

void sub_pthread_exit(void)
{
    int i;
    for (i = 0; i < MAX_CPU_NUMBER; i++)
    {
        munmap(queue[i].sa, BUFFER_SIZE);
    }
}
//end about pthread:

int main()
{
    float* abuff = malloc(256 * 2400 * 4);
    float* bbuff = malloc(7676 * 2400 * 4);
    float* cbuff = malloc(256 * 7676 * 4);
    int fd_a, fd_b;

    struct timespec start, finish;
    double elapsed;
    
    sub_pthread_init();

    if((fd_a = fopen("./demo_2nd_conv_A","rb")) ==-1)
    {
        printf("A creat file wrong!");
    }
    if((fd_b = fopen("./demo_2nd_conv_B","rb")) ==-1)
    {
        printf("B creat file wrong!");
    }
    printf("A read size:%d \r\n",  fread(abuff, 4, 256 * 2400, fd_a));
    printf("B read size:%d  \r\n", fread(bbuff, 4, 7676 * 2400, fd_b));
    close(fd_a);
    close(fd_b);
    printf("Aabuff:%x  Bbbuff:%x Ccbuff:%x \r\n",abuff,bbuff,cbuff);
    clock_gettime(CLOCK_MONOTONIC, &start);
    {
        sgemm_thread_nn(abuff, bbuff, cbuff, 256, 7676, 2400);
    }
    clock_gettime(CLOCK_MONOTONIC, &finish);
    elapsed = (finish.tv_sec - start.tv_sec);
    elapsed += (finish.tv_nsec - start.tv_nsec) / 1000000000.0;
    printf("sgemm_thread_nn elapsed time:%f cbuff[128*7676]:%f\r\n",elapsed,cbuff[128*7676]);

    free(abuff);
    free(bbuff);
    free(cbuff);
    
    sub_pthread_exit();
    return 0;
}
