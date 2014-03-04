/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2013-2014  Intel Corporation. All rights reserved.
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdbool.h>
#include <errno.h>
#include <glib.h>

#include "btio/btio.h"
#include "lib/bluetooth.h"
#include "lib/sdp.h"
#include "lib/sdp_lib.h"
#include "src/sdp-client.h"
#include "src/log.h"

#include "avctp.h"
#include "avrcp-lib.h"
#include "hal-msg.h"
#include "ipc-common.h"
#include "ipc.h"
#include "bluetooth.h"
#include "avrcp.h"

#define L2CAP_PSM_AVCTP 0x17

#define AVRCP_FEATURE_CATEGORY_1	0x0001
#define AVRCP_FEATURE_CATEGORY_2	0x0002
#define AVRCP_FEATURE_CATEGORY_3	0x0004
#define AVRCP_FEATURE_CATEGORY_4	0x0008

static bdaddr_t adapter_addr;
static uint32_t record_id = 0;
static GSList *devices = NULL;
static GIOChannel *server = NULL;
static struct ipc *hal_ipc = NULL;

struct avrcp_request {
	struct avrcp_device *dev;
	uint8_t pdu_id;
	uint8_t event_id;
	uint8_t transaction;
};

struct avrcp_device {
	bdaddr_t	dst;
	uint16_t	version;
	uint16_t	features;
	struct avrcp	*session;
	GIOChannel	*io;
	GQueue		*queue;
};

static struct avrcp_request *pop_request(uint8_t pdu_id, uint8_t event_id,
								bool peek)
{
	GSList *l;

	for (l = devices; l; l = g_slist_next(l)) {
		struct avrcp_device *dev = l->data;
		GList *reqs = g_queue_peek_head_link(dev->queue);
		int i;

		for (i = 0; reqs; reqs = g_list_next(reqs), i++) {
			struct avrcp_request *req = reqs->data;

			if (req->pdu_id != pdu_id || req->event_id != event_id)
				continue;

			if (!peek)
				g_queue_pop_nth(dev->queue, i);

			return req;
		}
	}

	return NULL;
}

static void handle_get_play_status(const void *buf, uint16_t len)
{
	const struct hal_cmd_avrcp_get_play_status *cmd = buf;
	uint8_t status;
	struct avrcp_request *req;
	int ret;

	DBG("");

	req = pop_request(AVRCP_GET_PLAY_STATUS, 0, false);
	if (!req) {
		status = HAL_STATUS_FAILED;
		goto done;
	}

	ret = avrcp_get_play_status_rsp(req->dev->session, req->transaction,
					cmd->position, cmd->duration,
					cmd->status);
	if (ret < 0) {
		status = HAL_STATUS_FAILED;
		g_free(req);
		goto done;
	}

	status = HAL_STATUS_SUCCESS;
	g_free(req);

done:
	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_AVRCP,
				HAL_OP_AVRCP_GET_PLAY_STATUS, status);
}

static void handle_list_player_attrs(const void *buf, uint16_t len)
{
	DBG("");

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_AVRCP,
			HAL_OP_AVRCP_LIST_PLAYER_ATTRS, HAL_STATUS_FAILED);
}

static void handle_list_player_values(const void *buf, uint16_t len)
{
	DBG("");

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_AVRCP,
			HAL_OP_AVRCP_LIST_PLAYER_VALUES, HAL_STATUS_FAILED);
}

static void handle_get_player_attrs(const void *buf, uint16_t len)
{
	DBG("");

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_AVRCP,
			HAL_OP_AVRCP_GET_PLAYER_ATTRS, HAL_STATUS_FAILED);
}

static void handle_get_player_attrs_text(const void *buf, uint16_t len)
{
	DBG("");

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_AVRCP,
			HAL_OP_AVRCP_GET_PLAYER_ATTRS_TEXT, HAL_STATUS_FAILED);
}

static void handle_get_player_values_text(const void *buf, uint16_t len)
{
	DBG("");

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_AVRCP,
			HAL_OP_AVRCP_GET_PLAYER_VALUES_TEXT, HAL_STATUS_FAILED);
}

static size_t write_element_text(uint8_t id, uint8_t text_len, uint8_t *text,
						uint8_t *pdu)
{
	uint16_t charset = 106;
	size_t len = 0;

	bt_put_be32(id, pdu);
	pdu += 4;
	len += 4;

	bt_put_be16(charset, pdu);
	pdu += 2;
	len += 2;

	bt_put_be16(text_len, pdu);
	pdu += 2;
	len += 2;

	memcpy(pdu, text, text_len);
	len += text_len;

	return len;
}

static void write_element_attrs(uint8_t *ptr, uint8_t number, uint8_t *pdu,
								size_t *len)
{
	int i;

	*pdu = number;
	pdu++;
	*len += 1;

	for (i = 0; i < number; i++) {
		struct hal_avrcp_player_setting_text *text = (void *) ptr;
		size_t ret;

		ret = write_element_text(text->id, text->len, text->text, pdu);

		ptr += sizeof(*text) + text->len;
		pdu += ret;
		*len += ret;
	}
}

static void handle_get_element_attrs_text(const void *buf, uint16_t len)
{
	struct hal_cmd_avrcp_get_element_attrs_text *cmd = (void *) buf;
	uint8_t status;
	struct avrcp_request *req;
	uint8_t pdu[IPC_MTU];
	uint8_t *ptr;
	size_t pdu_len;
	int ret;

	DBG("");

	req = pop_request(AVRCP_GET_ELEMENT_ATTRIBUTES, 0, false);
	if (!req) {
		status = HAL_STATUS_FAILED;
		goto done;
	}

	ptr = (uint8_t *) &cmd->values[0];
	pdu_len = 0;
	write_element_attrs(ptr, cmd->number, pdu, &pdu_len);

	ret = avrcp_get_element_attrs_rsp(req->dev->session, req->transaction,
								pdu, pdu_len);
	if (ret < 0) {
		status = HAL_STATUS_FAILED;
		g_free(req);
		goto done;
	}

	status = HAL_STATUS_SUCCESS;
	g_free(req);

done:
	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_AVRCP,
			HAL_OP_AVRCP_GET_ELEMENT_ATTRS_TEXT, status);
}

static void handle_set_player_attrs_value(const void *buf, uint16_t len)
{
	DBG("");

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_AVRCP,
			HAL_OP_AVRCP_SET_PLAYER_ATTRS_VALUE, HAL_STATUS_FAILED);
}

static void handle_register_notification(const void *buf, uint16_t len)
{
	struct hal_cmd_avrcp_register_notification *cmd = (void *) buf;
	uint8_t status;
	struct avrcp_request *req;
	uint8_t pdu[IPC_MTU];
	size_t pdu_len;
	uint8_t code;
	bool peek = false;
	int ret;

	DBG("");

	switch (cmd->type) {
	case HAL_AVRCP_EVENT_TYPE_INTERIM:
		code = AVC_CTYPE_INTERIM;
		peek = true;
		break;
	case HAL_AVRCP_EVENT_TYPE_CHANGED:
		code = AVC_CTYPE_CHANGED;
		break;
	default:
		status = HAL_STATUS_FAILED;
		goto done;
	}

	req = pop_request(AVRCP_REGISTER_NOTIFICATION, cmd->event, peek);
	if (!req) {
		status = HAL_STATUS_FAILED;
		goto done;
	}

	pdu[0] = cmd->event;
	pdu_len = 1;

	switch (cmd->event) {
	case AVRCP_EVENT_STATUS_CHANGED:
	case AVRCP_EVENT_TRACK_CHANGED:
	case AVRCP_EVENT_PLAYBACK_POS_CHANGED:
		memcpy(&pdu[1], cmd->data, cmd->len);
		pdu_len += cmd->len;
		break;
	default:
		status = HAL_STATUS_FAILED;
		goto done;
	}

	ret = avrcp_register_notification_rsp(req->dev->session,
						req->transaction, code,
						pdu, pdu_len);
	if (ret < 0) {
		status = HAL_STATUS_FAILED;
		if (!peek)
			g_free(req);
		goto done;
	}

	status = HAL_STATUS_SUCCESS;
	if (!peek)
		g_free(req);

done:
	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_AVRCP,
			HAL_OP_AVRCP_REGISTER_NOTIFICATION, status);
}

static void handle_set_volume(const void *buf, uint16_t len)
{
	DBG("");

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_AVRCP, HAL_OP_AVRCP_SET_VOLUME,
							HAL_STATUS_FAILED);
}

static const struct ipc_handler cmd_handlers[] = {
	/* HAL_OP_AVRCP_GET_PLAY_STATUS */
	{ handle_get_play_status, false,
			sizeof(struct hal_cmd_avrcp_get_play_status) },
	/* HAL_OP_AVRCP_LIST_PLAYER_ATTRS */
	{ handle_list_player_attrs, true,
			sizeof(struct hal_cmd_avrcp_list_player_attrs) },
	/* HAL_OP_AVRCP_LIST_PLAYER_VALUES */
	{ handle_list_player_values, true,
			sizeof(struct hal_cmd_avrcp_list_player_values) },
	/* HAL_OP_AVRCP_GET_PLAYER_ATTRS */
	{ handle_get_player_attrs, true,
			sizeof(struct hal_cmd_avrcp_get_player_attrs) },
	/* HAL_OP_AVRCP_GET_PLAYER_ATTRS_TEXT */
	{ handle_get_player_attrs_text, true,
			sizeof(struct hal_cmd_avrcp_get_player_attrs_text) },
	/* HAL_OP_AVRCP_GET_PLAYER_VALUES_TEXT */
	{ handle_get_player_values_text, true,
			sizeof(struct hal_cmd_avrcp_get_player_values_text) },
	/* HAL_OP_AVRCP_GET_ELEMENT_ATTRS_TEXT */
	{ handle_get_element_attrs_text, true,
			sizeof(struct hal_cmd_avrcp_get_element_attrs_text) },
	/* HAL_OP_AVRCP_SET_PLAYER_ATTRS_VALUE */
	{ handle_set_player_attrs_value, true,
			sizeof(struct hal_cmd_avrcp_set_player_attrs_value) },
	/* HAL_OP_AVRCP_REGISTER_NOTIFICATION */
	{ handle_register_notification, true,
			sizeof(struct hal_cmd_avrcp_register_notification) },
	/* HAL_OP_AVRCP_SET_VOLUME */
	{ handle_set_volume, false, sizeof(struct hal_cmd_avrcp_set_volume) },
};

static sdp_record_t *avrcp_record(void)
{
	sdp_list_t *svclass_id, *pfseq, *apseq, *root;
	uuid_t root_uuid, l2cap, avctp, avrtg;
	sdp_profile_desc_t profile[1];
	sdp_list_t *aproto_control, *proto_control[2];
	sdp_record_t *record;
	sdp_data_t *psm, *version, *features;
	uint16_t lp = L2CAP_PSM_AVCTP;
	uint16_t avrcp_ver = 0x0103, avctp_ver = 0x0103;
	uint16_t feat = ( AVRCP_FEATURE_CATEGORY_1 |
					AVRCP_FEATURE_CATEGORY_2 |
					AVRCP_FEATURE_CATEGORY_3 |
					AVRCP_FEATURE_CATEGORY_4);

	record = sdp_record_alloc();
	if (!record)
		return NULL;

	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root = sdp_list_append(NULL, &root_uuid);
	sdp_set_browse_groups(record, root);

	/* Service Class ID List */
	sdp_uuid16_create(&avrtg, AV_REMOTE_TARGET_SVCLASS_ID);
	svclass_id = sdp_list_append(NULL, &avrtg);
	sdp_set_service_classes(record, svclass_id);

	/* Protocol Descriptor List */
	sdp_uuid16_create(&l2cap, L2CAP_UUID);
	proto_control[0] = sdp_list_append(NULL, &l2cap);
	psm = sdp_data_alloc(SDP_UINT16, &lp);
	proto_control[0] = sdp_list_append(proto_control[0], psm);
	apseq = sdp_list_append(NULL, proto_control[0]);

	sdp_uuid16_create(&avctp, AVCTP_UUID);
	proto_control[1] = sdp_list_append(NULL, &avctp);
	version = sdp_data_alloc(SDP_UINT16, &avctp_ver);
	proto_control[1] = sdp_list_append(proto_control[1], version);
	apseq = sdp_list_append(apseq, proto_control[1]);

	aproto_control = sdp_list_append(NULL, apseq);
	sdp_set_access_protos(record, aproto_control);

	/* Bluetooth Profile Descriptor List */
	sdp_uuid16_create(&profile[0].uuid, AV_REMOTE_PROFILE_ID);
	profile[0].version = avrcp_ver;
	pfseq = sdp_list_append(NULL, &profile[0]);
	sdp_set_profile_descs(record, pfseq);

	features = sdp_data_alloc(SDP_UINT16, &feat);
	sdp_attr_add(record, SDP_ATTR_SUPPORTED_FEATURES, features);

	sdp_set_info_attr(record, "AVRCP TG", NULL, NULL);

	sdp_data_free(psm);
	sdp_data_free(version);
	sdp_list_free(proto_control[0], NULL);
	sdp_list_free(proto_control[1], NULL);
	sdp_list_free(apseq, NULL);
	sdp_list_free(aproto_control, NULL);
	sdp_list_free(pfseq, NULL);
	sdp_list_free(root, NULL);
	sdp_list_free(svclass_id, NULL);

	return record;
}

static void avrcp_device_free(void *data)
{
	struct avrcp_device *dev = data;

	g_queue_foreach(dev->queue, (GFunc) g_free, NULL);
	g_queue_free(dev->queue);

	if (dev->session)
		avrcp_shutdown(dev->session);

	if (dev->io) {
		g_io_channel_shutdown(dev->io, FALSE, NULL);
		g_io_channel_unref(dev->io);
	}

	g_free(dev);
}

static void avrcp_device_remove(struct avrcp_device *dev)
{
	devices = g_slist_remove(devices, dev);
	avrcp_device_free(dev);
}

static struct avrcp_device *avrcp_device_new(const bdaddr_t *dst)
{
	struct avrcp_device *dev;

	dev = g_new0(struct avrcp_device, 1);
	bacpy(&dev->dst, dst);
	devices = g_slist_prepend(devices, dev);

	return dev;
}

static int device_cmp(gconstpointer s, gconstpointer user_data)
{
	const struct avrcp_device *dev = s;
	const bdaddr_t *dst = user_data;

	return bacmp(&dev->dst, dst);
}

static struct avrcp_device *avrcp_device_find(const bdaddr_t *dst)
{
	GSList *l;

	l = g_slist_find_custom(devices, dst, device_cmp);
	if (!l)
		return NULL;

	return l->data;
}

static void disconnect_cb(void *data)
{
	struct avrcp_device *dev = data;

	DBG("");

	dev->session = NULL;

	avrcp_device_remove(dev);
}

static bool handle_fast_forward(struct avrcp *session, bool pressed,
							void *user_data)
{
	struct hal_ev_avrcp_passthrough_cmd ev;

	DBG("pressed %s", pressed ? "true" : "false");

	ev.id = AVC_FAST_FORWARD;
	ev.state = pressed;

	ipc_send_notif(hal_ipc, HAL_SERVICE_ID_AVRCP,
			HAL_EV_AVRCP_PASSTHROUGH_CMD, sizeof(ev), &ev);

	return true;
}

static bool handle_rewind(struct avrcp *session, bool pressed,
							void *user_data)
{
	struct hal_ev_avrcp_passthrough_cmd ev;

	DBG("pressed %s", pressed ? "true" : "false");

	ev.id = AVC_REWIND;
	ev.state = pressed;

	ipc_send_notif(hal_ipc, HAL_SERVICE_ID_AVRCP,
			HAL_EV_AVRCP_PASSTHROUGH_CMD, sizeof(ev), &ev);

	return true;
}

static const struct avrcp_passthrough_handler passthrough_handlers[] = {
		{ AVC_FAST_FORWARD, handle_fast_forward },
		{ AVC_REWIND, handle_rewind },
		{ },
};

static ssize_t handle_get_capabilities_cmd(struct avrcp *session,
						uint8_t transaction,
						uint16_t params_len,
						uint8_t *params,
						void *user_data)
{
	DBG("");

	if (params_len != 1)
		return -EINVAL;

	switch (params[0]) {
	case CAP_COMPANY_ID:
		params[params_len++] = 1;
		hton24(&params[params_len], IEEEID_BTSIG);
		return params_len + 3;
	case CAP_EVENTS_SUPPORTED:
		/* Android do not provide this info via HAL so the list most
		 * be hardcoded according to what RegisterNotification can
		 * actually handle */
		params[params_len++] = 3;
		params[params_len++] = AVRCP_EVENT_STATUS_CHANGED;
		params[params_len++] = AVRCP_EVENT_TRACK_CHANGED;
		params[params_len++] = AVRCP_EVENT_PLAYBACK_POS_CHANGED;
		return params_len;
	}

	return -EINVAL;
}

static void push_request(struct avrcp_device *dev, uint8_t pdu_id,
					uint8_t event_id, uint8_t transaction)
{
	struct avrcp_request *req;

	req = g_new0(struct avrcp_request, 1);
	req->dev = dev;
	req->pdu_id = pdu_id;
	req->event_id = event_id;
	req->transaction = transaction;

	g_queue_push_tail(dev->queue, req);
}

static ssize_t handle_get_play_status_cmd(struct avrcp *session,
						uint8_t transaction,
						uint16_t params_len,
						uint8_t *params,
						void *user_data)
{
	struct avrcp_device *dev = user_data;

	DBG("");

	if (params_len != 0)
		return -EINVAL;

	ipc_send_notif(hal_ipc, HAL_SERVICE_ID_AVRCP,
					HAL_EV_AVRCP_GET_PLAY_STATUS, 0, NULL);

	push_request(dev, AVRCP_GET_PLAY_STATUS, 0, transaction);

	return -EAGAIN;
}

static ssize_t handle_get_element_attrs_cmd(struct avrcp *session,
						uint8_t transaction,
						uint16_t params_len,
						uint8_t *params,
						void *user_data)
{
	struct avrcp_device *dev = user_data;
	uint8_t buf[IPC_MTU];
	struct hal_ev_avrcp_get_element_attrs *ev = (void *) buf;
	int i;

	DBG("");

	if (params_len < 9)
		return -EINVAL;

	ev->number = params[8];

	if (params_len < ev->number * sizeof(uint32_t) + 1)
		return -EINVAL;

	params += 9;
	for (i = 0; i < ev->number; i++) {
		ev->attrs[i] = bt_get_be32(params);
		params += 4;
	}

	ipc_send_notif(hal_ipc, HAL_SERVICE_ID_AVRCP,
					HAL_EV_AVRCP_GET_ELEMENT_ATTRS,
					sizeof(*ev) + ev->number, ev);

	push_request(dev, AVRCP_GET_ELEMENT_ATTRIBUTES, 0, transaction);

	return -EAGAIN;

}

static ssize_t handle_register_notification_cmd(struct avrcp *session,
						uint8_t transaction,
						uint16_t params_len,
						uint8_t *params,
						void *user_data)
{
	struct avrcp_device *dev = user_data;
	struct hal_ev_avrcp_register_notification ev;
	uint8_t event_id;

	DBG("");

	if (params_len != 5)
		return -EINVAL;

	event_id = params[0];

	/* TODO: Add any missing events supported by Android */
	switch (event_id) {
	case AVRCP_EVENT_STATUS_CHANGED:
	case AVRCP_EVENT_TRACK_CHANGED:
	case AVRCP_EVENT_PLAYBACK_POS_CHANGED:
		break;
	default:
		return -EINVAL;
	}

	ev.event = event_id;
	ev.param = bt_get_be32(&params[1]);

	ipc_send_notif(hal_ipc, HAL_SERVICE_ID_AVRCP,
					HAL_EV_AVRCP_REGISTER_NOTIFICATION,
					sizeof(ev), &ev);

	push_request(dev, AVRCP_REGISTER_NOTIFICATION, event_id, transaction);

	return -EAGAIN;
}

static const struct avrcp_control_handler control_handlers[] = {
		{ AVRCP_GET_CAPABILITIES,
					AVC_CTYPE_STATUS, AVC_CTYPE_STABLE,
					handle_get_capabilities_cmd },
		{ AVRCP_GET_PLAY_STATUS,
					AVC_CTYPE_STATUS, AVC_CTYPE_STABLE,
					handle_get_play_status_cmd },
		{ AVRCP_GET_ELEMENT_ATTRIBUTES,
					AVC_CTYPE_STATUS, AVC_CTYPE_STABLE,
					handle_get_element_attrs_cmd },
		{ AVRCP_REGISTER_NOTIFICATION,
					AVC_CTYPE_NOTIFY, AVC_CTYPE_INTERIM,
					handle_register_notification_cmd },
		{ },
};

static int avrcp_device_add_session(struct avrcp_device *dev, int fd,
						uint16_t imtu, uint16_t omtu)
{
	char address[18];

	dev->session = avrcp_new(fd, imtu, omtu, dev->version);
	if (!dev->session)
		return -EINVAL;

	avrcp_set_destroy_cb(dev->session, disconnect_cb, dev);
	avrcp_set_passthrough_handlers(dev->session, passthrough_handlers,
									dev);
	avrcp_set_control_handlers(dev->session, control_handlers, dev);

	dev->queue = g_queue_new();

	ba2str(&dev->dst, address);

	/* FIXME: get the real name of the device */
	avrcp_init_uinput(dev->session, "bluetooth", address);

	return 0;
}

static void connect_cb(GIOChannel *chan, GError *err, gpointer user_data)
{
	struct avrcp_device *dev = user_data;
	uint16_t imtu, omtu;
	char address[18];
	GError *gerr = NULL;
	int fd;

	if (err) {
		error("%s", err->message);
		return;
	}

	bt_io_get(chan, &gerr,
			BT_IO_OPT_DEST, address,
			BT_IO_OPT_IMTU, &imtu,
			BT_IO_OPT_OMTU, &omtu,
			BT_IO_OPT_INVALID);
	if (gerr) {
		error("%s", gerr->message);
		g_error_free(gerr);
		g_io_channel_shutdown(chan, TRUE, NULL);
		return;
	}

	fd = g_io_channel_unix_get_fd(chan);
	if (avrcp_device_add_session(dev, fd, imtu, omtu) < 0) {
		avrcp_device_free(dev);
		return;
	}

	g_io_channel_set_close_on_unref(chan, FALSE);

	if (dev->io) {
		g_io_channel_unref(dev->io);
		dev->io = NULL;
	}

	DBG("%s connected", address);
}

static bool avrcp_device_connect(struct avrcp_device *dev, BtIOConnect cb)
{
	GError *err = NULL;

	dev->io = bt_io_connect(cb, dev, NULL, &err,
					BT_IO_OPT_SOURCE_BDADDR, &adapter_addr,
					BT_IO_OPT_DEST_BDADDR, &dev->dst,
					BT_IO_OPT_PSM, L2CAP_PSM_AVCTP,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
					BT_IO_OPT_INVALID);
	if (err) {
		error("%s", err->message);
		g_error_free(err);
		return false;
	}

	return true;
}

static void search_cb(sdp_list_t *recs, int err, gpointer data)
{
	struct avrcp_device *dev = data;
	sdp_list_t *list;

	DBG("");

	if (err < 0) {
		error("Unable to get AV_REMOTE_SVCLASS_ID SDP record: %s",
							strerror(-err));
		goto fail;
	}

	if (!recs || !recs->data) {
		error("No AVRCP records found");
		goto fail;
	}

	for (list = recs; list; list = list->next) {
		sdp_record_t *rec = list->data;
		sdp_data_t *data;

		data = sdp_data_get(rec, SDP_ATTR_VERSION);
		if (data)
			dev->version = data->val.uint16;

		data = sdp_data_get(rec, SDP_ATTR_SUPPORTED_FEATURES);
		if (data)
			dev->features = data->val.uint16;
	}

	if (dev->io) {
		GError *gerr = NULL;
		if (!bt_io_accept(dev->io, connect_cb, dev, NULL, &gerr)) {
			error("bt_io_accept: %s", gerr->message);
			g_error_free(gerr);
			goto fail;
		}
		return;
	}

	if (!avrcp_device_connect(dev, connect_cb)) {
		error("Unable to connect to AVRCP");
		goto fail;
	}

	return;

fail:
	avrcp_device_remove(dev);
}

static int avrcp_device_search(struct avrcp_device *dev)
{
	uuid_t uuid;

	sdp_uuid16_create(&uuid, AV_REMOTE_SVCLASS_ID);

	return bt_search_service(&adapter_addr, &dev->dst, &uuid, search_cb,
								dev, NULL, 0);
}

static void confirm_cb(GIOChannel *chan, gpointer data)
{
	struct avrcp_device *dev;
	char address[18];
	bdaddr_t src, dst;
	GError *err = NULL;

	bt_io_get(chan, &err,
			BT_IO_OPT_SOURCE_BDADDR, &src,
			BT_IO_OPT_DEST_BDADDR, &dst,
			BT_IO_OPT_DEST, address,
			BT_IO_OPT_INVALID);
	if (err) {
		error("%s", err->message);
		g_error_free(err);
		g_io_channel_shutdown(chan, TRUE, NULL);
		return;
	}

	DBG("incoming connect from %s", address);

	dev = avrcp_device_find(&dst);
	if (dev && dev->session) {
		error("AVRCP: Refusing unexpected connect");
		g_io_channel_shutdown(chan, TRUE, NULL);
		return;
	}

	dev = avrcp_device_new(&dst);
	if (avrcp_device_search(dev) < 0) {
		error("AVRCP: Failed to search SDP details");
		avrcp_device_free(dev);
		g_io_channel_shutdown(chan, TRUE, NULL);
	}

	dev->io = g_io_channel_ref(chan);
}

bool bt_avrcp_register(struct ipc *ipc, const bdaddr_t *addr, uint8_t mode)
{
	GError *err = NULL;
	sdp_record_t *rec;

	DBG("");

	bacpy(&adapter_addr, addr);

	server = bt_io_listen(NULL, confirm_cb, NULL, NULL, &err,
				BT_IO_OPT_SOURCE_BDADDR, &adapter_addr,
				BT_IO_OPT_PSM, L2CAP_PSM_AVCTP,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
				BT_IO_OPT_INVALID);
	if (!server) {
		error("Failed to listen on AVDTP channel: %s", err->message);
		g_error_free(err);
		return false;
	}

	rec = avrcp_record();
	if (!rec) {
		error("Failed to allocate AVRCP record");
		goto fail;
	}

	if (bt_adapter_add_record(rec, 0) < 0) {
		error("Failed to register AVRCP record");
		sdp_record_free(rec);
		goto fail;
	}
	record_id = rec->handle;

	hal_ipc = ipc;

	ipc_register(hal_ipc, HAL_SERVICE_ID_AVRCP, cmd_handlers,
						G_N_ELEMENTS(cmd_handlers));

	return true;
fail:
	g_io_channel_shutdown(server, TRUE, NULL);
	g_io_channel_unref(server);
	server = NULL;

	return false;
}

void bt_avrcp_unregister(void)
{
	DBG("");

	g_slist_free_full(devices, avrcp_device_free);
	devices = NULL;

	ipc_unregister(hal_ipc, HAL_SERVICE_ID_AVRCP);
	hal_ipc = NULL;

	bt_adapter_remove_record(record_id);
	record_id = 0;

	if (server) {
		g_io_channel_shutdown(server, TRUE, NULL);
		g_io_channel_unref(server);
		server = NULL;
	}
}

void bt_avrcp_connect(const bdaddr_t *dst)
{
	struct avrcp_device *dev;
	char addr[18];

	DBG("");

	if (avrcp_device_find(dst))
		return;

	dev = avrcp_device_new(dst);
	if (avrcp_device_search(dev) < 0) {
		error("AVRCP: Failed to search SDP details");
		avrcp_device_free(dev);
	}

	ba2str(&dev->dst, addr);
	DBG("connecting to %s", addr);
}

void bt_avrcp_disconnect(const bdaddr_t *dst)
{
	struct avrcp_device *dev;

	DBG("");

	dev = avrcp_device_find(dst);
	if (!dev)
		return;

	if (dev->session) {
		avrcp_shutdown(dev->session);
		return;
	}

	avrcp_device_remove(dev);
}
