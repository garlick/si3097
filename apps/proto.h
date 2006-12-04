/* lib.c */
int sendfile(int fd, int breaktime, char *filename);
void init_com(int fd, int baud, int parity, int bits, int stopbits, int buffersize);
int send_command(int fd, int cmd);
int clear_buffer(int fd);
int send_char(int fd, int data);
int receive_char(int fd);
int receive_n_ints(int fd, int n, int *data);
int send_n_ints(int fd, int n, int *data);
int swapl(int *d);
int expect_yn(int fd);
void send_break(int fd, int ms);
int load_camera_cfg(struct SI_CAMERA *c, char *fname);
int load_cfg(struct CFG_ENTRY **e, char *fname, char *var);
int parse_cfg_string(struct CFG_ENTRY *entry);
char *name_cfg(char *cfg);
int send_command_yn(int fd, int data);
int setfile_readout(struct SI_CAMERA *c, char *file);
struct CFG_ENTRY *find_readout(struct SI_CAMERA *c, char *name);
