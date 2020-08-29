/**
 * Nanobenchmark: Read operation
 *   RSF. PROCESS = {read a non-overlapping region of /test/test.file}
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#define __STDC_FORMAT_MACROS
#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>
#include <libaio.h>
#include <sys/stat.h>
#include "fxmark.h"
#include "util.h"

#define NBYTES 4096
#define NBUF   512

#define PRIVATE_REGION_SIZE (1024 * 1024 * 8)
#define PRIVATE_REGION_PAGE_NUM (PRIVATE_REGION_SIZE/PAGE_SIZE)

static void set_shared_test_root(struct worker *worker, char *test_root)
{
        struct fx_opt *fx_opt = fx_opt_worker(worker);
        sprintf(test_root, "%s", fx_opt->root);
}

static void set_test_file(struct worker *worker, char *test_root)
{
        struct fx_opt *fx_opt = fx_opt_worker(worker);
        sprintf(test_root, "%s/n_shfile_rd.dat", fx_opt->root);
}

static int pre_work(struct worker *worker)
{
        char *page=NULL;
        struct bench *bench = worker->bench;
        char path[PATH_MAX];
        int fd, max_id = -1, rc;
        int i, j;

        /*Allocate aligned buffer*/
        if(posix_memalign((void **)&(worker->page), PAGE_SIZE, PAGE_SIZE))
                goto err_out;
        page = worker->page;
        if (!page)
                goto err_out;

        /* a leader takes over all pre_work() */
        if (worker->id != 0)
                return 0;

        /* find the largest worker id */
        for (i = 0; i < bench->ncpu; ++i) {
                struct worker *w = &bench->workers[i];
                if (w->id > max_id)
                        max_id = w->id;
        }

        /* create a test file */
        set_shared_test_root(worker, path);
        rc = mkdir_p(path);
        if (rc) return rc;

        set_test_file(worker, path);
        if ((fd = open(path, O_CREAT | O_RDWR, S_IRWXU)) == -1)
                goto err_out;

        /*set flag with O_DIRECT if necessary*/
        if(bench->directio && (fcntl(fd, F_SETFL, O_DIRECT)==-1))
                goto err_out;

        for (i = 0; i <= max_id; ++i) {
                for (j = 0; j < PRIVATE_REGION_PAGE_NUM; ++j) {
                        if (write(fd, page, PAGE_SIZE) != PAGE_SIZE)
                                goto err_out;
                }
        }
        fsync(fd);
        close(fd);
out:
        return rc;
err_out:
        rc = errno;
        if(page)
                free(page);
        goto out;
}
// pread(int fd, void *buf, size_t count, off_t offset);
// preadv2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags);
static int main_work(struct worker *worker)
{       
        int j, k, nbytes = PAGE_SIZE, maxevents = PATH_MAX;
        char *buf[PATH_MAX];
        struct iocb *iocbray[PATH_MAX], *iocb;
        off_t offset;
        io_context_t ctx = 0;
        struct io_event events[2 * PATH_MAX];
        struct timespec timeout = { 0, 0 };

        struct bench *bench = worker->bench;
        char *page = worker->page;
        
        char path[PATH_MAX];
        int fd, rc = 0;
        off_t pos;
        uint64_t iter = 0;

        assert(page);

        set_test_file(worker, path);
        if ((fd = open(path, O_CREAT | O_RDWR, S_IRWXU)) == -1)
                goto err_out;

        /* set flag with O_DIRECT if necessary*/
        if(bench->directio && (fcntl(fd, F_SETFL, O_DIRECT)==-1))
                goto err_out;

        for (j = 0; j < PATH_MAX; j++) {
                /* no need to zero iocbs; will be done in io_prep_pread */
                iocbray[j] = malloc(sizeof(struct iocb));
                buf[j] = malloc(PAGE_SIZE);
                memset(buf[j], 0, PAGE_SIZE);
        }
        
        rc = io_setup(maxevents, &ctx);

        pos = PRIVATE_REGION_SIZE * worker->id;
        for (j=0,iter = 0; !bench->stop; j++,++iter) {
                if (j == PATH_MAX) j = 0;
                iocb = iocbray[j];
                // offset = iter * nbytes;
                // printf("read\n");
                // io_prep_pread(iocb, fd, page, PAGE_SIZE, pos);
                pos += PAGE_SIZE;
                if (pos - PRIVATE_REGION_SIZE){
                        pos = PRIVATE_REGION_SIZE * worker->id;
                }
                // printf("pread completed");
                // rc = io_submit(ctx, 1, &iocb);
                if (pread(fd, page, PAGE_SIZE, pos) != PAGE_SIZE)
                        goto err_out;
        }
        while ((rc = io_getevents(ctx, PATH_MAX, PATH_MAX, events, &timeout)) > 0) {
                //printf(" rc from io_getevents on the read = %d\n\n", rc);
        }
        rc = io_destroy(ctx);
        close(fd);
out:
        worker->works = (double)iter;
        return rc;
err_out:
        bench->stop = 1;
        rc = errno;
        free(page);
        goto out;
}

struct bench_operations n_shfile_rd_ops = {
        .pre_work  = pre_work,
        .main_work = main_work,
};
