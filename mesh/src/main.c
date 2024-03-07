/* main.c - Application main entry point */

/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/printk.h>

#include <zephyr/settings/settings.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>

#include "board.h"

extern void spi_data_get();
extern uint8_t raw_data[3072];

int debugbuf(uint8_t* buf,int size);
int packetcount = 0 ;//已经传输的数据包的量
static uint16_t packetnumber = 0;//需要传输的数据包的量

#define PAYLOAD_SIZE 28 //每个数据包真正载着的data size
#define THE_PT_ID	0X03
#define DATASIZE 3072//测试用，数据的大小

char _tmp[DATASIZE] = {0};
static uint16_t length = sizeof(_tmp);
//static uint8_t tx_buf[5] =  {0x01,0x02,0x03,0x04,0x04};
int count_10ms = 0;
int count_60ms = 0;
int send_err;

/* uart */
const struct device *uart1 = DEVICE_DT_GET(DT_NODELABEL(uart1));
const struct uart_config uart_cfg = {
	.baudrate = 115200,
	.parity = UART_CFG_PARITY_NONE,
	.stop_bits = UART_CFG_STOP_BITS_1,
	.data_bits = UART_CFG_DATA_BITS_8,
	.flow_ctrl = UART_CFG_FLOW_CTRL_NONE
};

#define OP_ONOFF_GET       BT_MESH_MODEL_OP_2(0x82, 0x01)
#define OP_ONOFF_SET       BT_MESH_MODEL_OP_2(0x82, 0x02)
#define OP_ONOFF_SET_UNACK BT_MESH_MODEL_OP_2(0x82, 0x03)
#define OP_ONOFF_STATUS    BT_MESH_MODEL_OP_2(0x82, 0x04)

static void attention_on(struct bt_mesh_model *mod)
{
	board_led_set(true);
}

static void attention_off(struct bt_mesh_model *mod)
{
	board_led_set(false);
}

static const struct bt_mesh_health_srv_cb health_cb = {
	.attn_on = attention_on,
	.attn_off = attention_off,
};

static struct bt_mesh_health_srv health_srv = {
	.cb = &health_cb,
};

BT_MESH_HEALTH_PUB_DEFINE(health_pub, 0);

static const char *const onoff_str[] = { "off", "on" };

static struct {
	bool val;
	uint8_t tid;
	uint16_t src;
	uint32_t transition_time;
	struct k_work_delayable work;
} onoff;

/* OnOff messages' transition time and remaining time fields are encoded as an
 * 8 bit value with a 6 bit step field and a 2 bit resolution field.
 * The resolution field maps to:
 * 0: 100 ms
 * 1: 1 s
 * 2: 10 s
 * 3: 20 min
 */
static const uint32_t time_res[] = {
	100,
	MSEC_PER_SEC,
	10 * MSEC_PER_SEC,
	10 * 60 * MSEC_PER_SEC,
};

static inline int32_t model_time_decode(uint8_t val)
{
	uint8_t resolution = (val >> 6) & BIT_MASK(2);
	uint8_t steps = val & BIT_MASK(6);

	if (steps == 0x3f) {
		return SYS_FOREVER_MS;
	}

	return steps * time_res[resolution];
}

static inline uint8_t model_time_encode(int32_t ms)
{
	if (ms == SYS_FOREVER_MS) {
		return 0x3f;
	}

	for (int i = 0; i < ARRAY_SIZE(time_res); i++) {
		if (ms >= BIT_MASK(6) * time_res[i]) {
			continue;
		}

		uint8_t steps = ceiling_fraction(ms, time_res[i]);

		return steps | (i << 6);
	}

	return 0x3f;
}
//将led的状态发给各个node
static int onoff_status_send(struct bt_mesh_model *model,
			     struct bt_mesh_msg_ctx *ctx)
{
	uint32_t remaining;
	//定义了一个3b缓存区，初始化为OP_ONOFF_STATUS
	BT_MESH_MODEL_BUF_DEFINE(buf, OP_ONOFF_STATUS, 3);
	bt_mesh_model_msg_init(&buf, OP_ONOFF_STATUS);

	remaining = k_ticks_to_ms_floor32(
			    k_work_delayable_remaining_get(&onoff.work)) +
		    onoff.transition_time;

	/* Check using remaining time instead of "work pending" to make the
	 * onoff status send the right value on instant transitions. As the
	 * work item is executed in a lower priority than the mesh message
	 * handler, the work will be pending even on instant transitions.
	 */
	if (remaining) {
		net_buf_simple_add_u8(&buf, !onoff.val);
		net_buf_simple_add_u8(&buf, onoff.val);
		net_buf_simple_add_u8(&buf, model_time_encode(remaining));
	} else {
		net_buf_simple_add_u8(&buf, onoff.val);
	}

	return bt_mesh_model_send(model, ctx, &buf, NULL, NULL);
}
//这段代码是一个定时器回调函数，在蓝牙Mesh协议栈中的应用场景是，当一个蓝牙Mesh节点需要改变一个Generic OnOff Model的状态时，
//这个回调函数就会被注册到一个Kernal Work Queue中。回调函数中，首先判断当前是否有状态转换的任务需要执行，如果有，那么就会开始状态转换，
//即LED会一直亮着，直到状态转换任务执行完毕。这里的LED可以被理解为是代表实际物理设备的一个灯，用来展示当前设备的状态。如果没有状态转换任务需要执行，
//那么LED就会被设置为当前的状态值。这个回调函数主要是用来控制物理设备的状态和LED的状态，并且在状态转换的过程中保持LED一直亮着
/* static void onoff_timeout(struct k_work *work)
{
	if (onoff.transition_time) {
		* Start transition.
		 *
		 * The LED should be on as long as the transition is in
		 * progress, regardless of the target value, according to the
		 * Bluetooth Mesh Model specification, section 3.1.1.
		 *
		board_led_set(true);

		k_work_reschedule(&onoff.work, K_MSEC(onoff.transition_time));
		onoff.transition_time = 0;
		return;
	}

	board_led_set(onoff.val);
} 
*/

/* Generic OnOff Server message handlers */

static int gen_onoff_get(struct bt_mesh_model *model,
			 struct bt_mesh_msg_ctx *ctx,
			 struct net_buf_simple *buf)
{
	onoff_status_send(model, ctx);
	return 0;
}

static int gen_onoff_set_unack(struct bt_mesh_model *model,
			       struct bt_mesh_msg_ctx *ctx,
			       struct net_buf_simple *buf)
{
	//表示接收到的，先把前两个byte pull出去，一个是onoff一个是tid标识

/* 	uint8_t pt_id = net_buf_simple_pull_u8(buf);
	uint8_t count = net_buf_simple_pull_u8(buf); */
	//uint8_t val = net_buf_simple_pull_u8(buf);
	//uint8_t tid = net_buf_simple_pull_u8(buf);
	//int32_t trans = 0;
	//int32_t delay = 0;

/* 	printk("====================================\n");
	printk("the total data size is :%d\n",length);
	//printk("the current packet size is :%d\n",buf->len);
	printk("the PT ID is :%02x\n",pt_id);
	printk("the current packet number is :%d\n",count+1); */

	if (buf->len) 
	{
		//trans = model_time_decode(net_buf_simple_pull_u8(buf));
		//delay = net_buf_simple_pull_u8(buf) * 5;
		//printk("the current packet data: \n");
		uint8_t len = buf->len;
		uint8_t tx_data[32] = {0};
		void* p = net_buf_simple_pull_mem(buf,len);
		memcpy(tx_data,p,len);
		printk("接收端信号强度%d\n",ctx->recv_rssi);
		uint8_t pt_id = tx_data[0];
		uint8_t count = tx_data[1];

/* 打印 */
/* 		printk("====================================\n");
		printk("the total data size is :%d\n",length);
		printk("the PT ID is :%02x\n",pt_id);
		printk("the current packet number is :%d\n",count+1); */

		int err = uart_tx(uart1, tx_data, len, SYS_FOREVER_US);
		if (err) {
			return 0;
		}
/* 打印 */
		//debugbuf(tx_data,len);
	}
	//printk("====================================\n");
	// if (buf->len) {
	// 	trans = model_time_decode(net_buf_simple_pull_u8(buf));
	// 	delay = net_buf_simple_pull_u8(buf) * 5;
	// }

	/* Only perform change if the message wasn't a duplicate and the
	 * value is different.
	 */

/*
	if (tid == onoff.tid && ctx->addr == onoff.src) {
		 Duplicate 
		return 0;
	}

	if (val == onoff.val) {
		 No change 
		return 0;
	}

	//printk("set: %s delay: %d ms time: %d ms\n", onoff_str[val], delay,
	       trans);


	onoff.tid = tid;
	onoff.src = ctx->addr;
	onoff.val = val;
	onoff.transition_time = trans;

	* Schedule the next action to happen on the delay, and keep
	 * transition time stored, so it can be applied in the timeout.
	 
	k_work_reschedule(&onoff.work, K_MSEC(delay));
*/

	return 0;
}

static int gen_onoff_set(struct bt_mesh_model *model,
			 struct bt_mesh_msg_ctx *ctx,
			 struct net_buf_simple *buf)
{
	(void)gen_onoff_set_unack(model, ctx, buf);
	onoff_status_send(model, ctx);
	return 0;
}

static const struct bt_mesh_model_op gen_onoff_srv_op[] = {
	{ OP_ONOFF_GET,       BT_MESH_LEN_EXACT(0), gen_onoff_get },
	{ OP_ONOFF_SET,       BT_MESH_LEN_MIN(2),   gen_onoff_set },
	{ OP_ONOFF_SET_UNACK, BT_MESH_LEN_MIN(2),   gen_onoff_set_unack },
	BT_MESH_MODEL_OP_END,
};

/* Generic OnOff Client */

static int gen_onoff_status(struct bt_mesh_model *model,
			    struct bt_mesh_msg_ctx *ctx,
			    struct net_buf_simple *buf)
{
	uint8_t present = net_buf_simple_pull_u8(buf);

	if (buf->len) {
		uint8_t target = net_buf_simple_pull_u8(buf);
		int32_t remaining_time =
			model_time_decode(net_buf_simple_pull_u8(buf));

		printk("OnOff status: %s -> %s: (%d ms)\n", onoff_str[present],
		       onoff_str[target], remaining_time);
		return 0;
	}

	printk("OnOff status: %s\n", onoff_str[present]);

	return 0;
}

static const struct bt_mesh_model_op gen_onoff_cli_op[] = {
	{OP_ONOFF_STATUS, BT_MESH_LEN_MIN(1), gen_onoff_status},
	BT_MESH_MODEL_OP_END,
};

/* This application only needs one element to contain its models */
static struct bt_mesh_model models[] = {
	BT_MESH_MODEL_CFG_SRV,
	BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
	BT_MESH_MODEL(BT_MESH_MODEL_ID_GEN_ONOFF_SRV, gen_onoff_srv_op, NULL,
		      NULL),
	BT_MESH_MODEL(BT_MESH_MODEL_ID_GEN_ONOFF_CLI, gen_onoff_cli_op, NULL,
		      NULL),
};

static struct bt_mesh_elem elements[] = {
	BT_MESH_ELEM(0, models, BT_MESH_MODEL_NONE),
};

static const struct bt_mesh_comp comp = {
	.cid = BT_COMP_ID_LF,
	.elem = elements,
	.elem_count = ARRAY_SIZE(elements),
};

/* Provisioning */

static int output_number(bt_mesh_output_action_t action, uint32_t number)
{
	printk("OOB Number: %u\n", number);

	board_output_number(action, number);

	return 0;
}

static void prov_complete(uint16_t net_idx, uint16_t addr)
{
	board_prov_complete();
}

static void prov_reset(void)
{
	bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
}

static uint8_t dev_uuid[16];

static const struct bt_mesh_prov prov = {
	.uuid = dev_uuid,
	.output_size = 4,
	.output_actions = BT_MESH_DISPLAY_NUMBER,
	.output_number = output_number,
	.complete = prov_complete,
	.reset = prov_reset,
};
/** Send an OnOff Set message from the Generic OnOff Client to all nodes. */
static int gen_onoff_send(bool val)
{

	struct bt_mesh_msg_ctx ctx = {
		.app_idx = models[3].keys[0], /* Use the bound key */
		.addr = 0x0002,//BT_MESH_ADDR_ALL_NODES,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};

	//printk("%d",BT_MESH_TTL_DEFAULT);
	//static uint8_t tid;
	if (ctx.app_idx == BT_MESH_KEY_UNUSED) {
		printk("The Generic OnOff Client must be bound to a key before "
		       "sending.\n");
		return -ENOENT;
	}

/* 	static bool onoff;
    if(onoff==1)
	{
		onoff = 0;
	}
	else
	{
		onoff = 1;
	} */

	BT_MESH_MODEL_BUF_DEFINE(buf, OP_ONOFF_SET_UNACK, 2 + 32 - 4);
	bt_mesh_model_msg_init(&buf, OP_ONOFF_SET_UNACK);


	//BT_MESH_MODEL_BUF_DEFINE(buf, OP_ONOFF_SET_UNACK, 2 + 32 - 4);
	//bt_mesh_model_msg_init(&buf, OP_ONOFF_SET_UNACK);

	//net_buf_simple_add_u8(&buf, onoff);
	//net_buf_simple_add_u8(&buf, tid++);

	//get the sensor data

	//char _tmp[32] = {0};
	//bt_rand(_tmp,32);

	//my thread
	uint16_t leftdata = 0;
    //uint16_t packetnumber;
    //uint8_t crc;

	//get the packetnumber
/*     if(length % PAYLOAD_SIZE == 0){
        packetnumber = length / PAYLOAD_SIZE;
    }else{
        packetnumber = length / PAYLOAD_SIZE + 1;
    } */

/* 打印 */
/* 	printk("----------------------------------\n");
    printk("the data size is : %d byte\n", length);
    printk("the total packet number is : %d \n", packetnumber);
 */
	if(packetcount < packetnumber){
		int startindex = PAYLOAD_SIZE*packetcount;
		leftdata = length - PAYLOAD_SIZE*packetcount;
        if(leftdata > PAYLOAD_SIZE){
            leftdata = PAYLOAD_SIZE;
        } else{
            leftdata = length - PAYLOAD_SIZE*packetcount;
        }
		net_buf_simple_add_u8(&buf, THE_PT_ID);
		net_buf_simple_add_u8(&buf, packetcount);
		net_buf_simple_add_mem(&buf,&_tmp[startindex],leftdata);
		/* 打印 */
		/* printk("the current packet number is : %d startindex %d leftdata %d \n", packetcount+1,startindex,leftdata);
		debugbuf(&_tmp[startindex],leftdata);
		printk("----------------------------------\n"); */
		send_err =  bt_mesh_model_send(&models[3], &ctx, &buf, NULL, NULL);
		
	}else{
		send_err =  bt_mesh_model_send(&models[3], &ctx, &buf, NULL, NULL);
	}

	//printk("the bt_mesh_model_send err code is %d",err);
	return 0;

	//net_buf_simple_add_mem(&buf,_tmp,32-4);
	//printk("----------------------------------\n");
	//debugbuf(_tmp,32-4);
	//printk("Sending OnOff Set: %s\n", onoff_str[onoff]);
	//printk("Sending OnOff Set: %s\n", onoff_str[val]);
	//return bt_mesh_model_send(&models[3], &ctx, &buf, NULL, NULL);
}

static void button_pressed(struct k_work *work)
{	
	if (bt_mesh_is_provisioned()) {
		//(void)gen_onoff_send(!onoff.val);
		return;
	}

	/* Self-provision with an arbitrary address.
	 *
	 * NOTE: This should never be done in a production environment.
	 *       Addresses should be assigned by a provisioner, and keys should
	 *       be generated from true random numbers. It is done in this
	 *       sample to allow testing without a provisioner.
	 */
	static uint8_t net_key[16];
	static uint8_t dev_key[16];
	static uint8_t app_key[16];
	uint16_t addr;
	int err;

	if (IS_ENABLED(CONFIG_HWINFO)) {
		addr = sys_get_le16(&dev_uuid[0]) & BIT_MASK(15);
	} else {
		addr = k_uptime_get_32() & BIT_MASK(15);
	}

	printk("Self-provisioning with address 0x%04x\n", addr);
	err = bt_mesh_provision(net_key, 0, 0, 0, addr, dev_key);
	if (err) {
		printk("Provisioning failed (err: %d)\n", err);
		return;
	}

	/* Add an application key to both Generic OnOff models: */
	err = bt_mesh_app_key_add(0, 0, app_key);
	if (err) {
		printk("App key add failed (err: %d)\n", err);
		return;
	}

	/* Models must be bound to an app key to send and receive messages with
	 * it:
	 */
	models[2].keys[0] = 0;
	models[3].keys[0] = 0;

	printk("Provisioned and configured!\n");
}

static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	err = bt_mesh_init(&prov, &comp);
	if (err) {
		printk("Initializing mesh failed (err %d)\n", err);
		return;
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	/* This will be a no-op if settings_load() loaded provisioning info */
	bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);

	printk("Mesh initialized\n");
}



int debugbuf(uint8_t* buf,int size)
{
	int i = 0;
	for (i=0;i<size;i++)
	{
		printk("%02X",buf[i]&0xff);

	}
	printk("\n");
	return 0;
}


void main(void)
{
	static struct k_work button_work;
	int err = -1;

	printk("Initializing...\n");

	if (IS_ENABLED(CONFIG_HWINFO)) {
		err = hwinfo_get_device_id(dev_uuid, sizeof(dev_uuid));
	}

	if (err < 0) {
		dev_uuid[0] = 0xdd;
		dev_uuid[1] = 0xdd;
	}

	k_work_init(&button_work, button_pressed);

	err = board_init(&button_work);
	if (err) {
		printk("Board init failed (err: %d)\n", err);
		return;
	}

	//k_work_init_delayable(&onoff.work, onoff_timeout);

	/* Initialize the Bluetooth Subsystem */
	err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
	}

	/* uart */
/* 	if (!device_is_ready(uart1)){
		return ;
	}
	err = uart_configure(uart1, &uart_cfg);
	if (err == -ENOSYS) {
		return ;
	} */

	spi_data_get();
	k_msleep(2000);  

 	for(int a = 0; a<DATASIZE;a++)
	{
		_tmp[a] = raw_data[a];
	}

	if(length % PAYLOAD_SIZE == 0){
        packetnumber = length / PAYLOAD_SIZE;
    }else{
        packetnumber = length / PAYLOAD_SIZE + 1;
    }

	button_pressed(&button_work);
	send_err = 0;

	while(packetcount<packetnumber)
	{	
	
		gen_onoff_send(!onoff.val);
		if(!send_err){
			packetcount++;
			//printk("10ms %d,60ms %d\n",count_10ms,count_60ms);
		}else{
			//printk("Sending failed. Retrying...\n");
			k_msleep(10);
			continue;
		}
		k_msleep(60);
	}

	/* duty-cycle */
/* 	while(packetcount<packetnumber)
	{	
		gen_onoff_send(!onoff.val);
		if(!send_err){
			packetcount++;
		}else{
			printk("Sending failed. Retrying...\n");
			k_msleep(6000);
			continue;
		}
		k_msleep(500);
	} */
/* 	while(1){
		spi_data_get();
		k_msleep(5000);
		//packetcount=0;
		for(int a = 0; a<DATASIZE;a++)
		{
			_tmp[a] = raw_data[a];
		}

		if(length % PAYLOAD_SIZE == 0){
        	packetnumber = length / PAYLOAD_SIZE;
    	}else{
        	packetnumber = length / PAYLOAD_SIZE + 1;
    	}

		while(packetcount<packetnumber)
		{
	
			button_pressed(&button_work);
			packetcount++;
			k_msleep(600);

		}
		k_msleep(6000);
	} */
}
