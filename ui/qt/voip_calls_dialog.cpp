/* voip_calls_dialog.cpp
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "voip_calls_dialog.h"
#include "ui_voip_calls_dialog.h"

#include "file.h"

#include "epan/addr_resolv.h"
#include "epan/dissectors/packet-h225.h"

#include "ui/utf8_entities.h"

#include "sequence_dialog.h"
#include "stock_icon.h"
#include "wireshark_application.h"

#include <QContextMenuEvent>
#include <QPushButton>

// To do:
// - More context menu items
//   - Don't select on right click
// - Player
// - Add a screenshot to the user's guide

// Bugs:
// - Preparing a filter overwrites the existing filter. The GTK+ UI appends.
//   We'll probably have to add an "append" parameter to MainWindow::filterPackets.

// VoipCallsTreeWidgetItem
// QTreeWidgetItem subclass that allows sorting

const int start_time_col_ = 0;
const int stop_time_col_ = 1;
const int initial_speaker_col_ = 2;
const int from_col_ = 3;
const int to_col_ = 4;
const int protocol_col_ = 5;
const int packets_col_ = 6;
const int state_col_ = 7;
const int comments_col_ = 8;

Q_DECLARE_METATYPE(voip_calls_info_t*)

class VoipCallsTreeWidgetItem : public QTreeWidgetItem
{
public:
    VoipCallsTreeWidgetItem(QTreeWidget *tree, voip_calls_info_t *call_info) : QTreeWidgetItem(tree) {
        setData(0, Qt::UserRole, qVariantFromValue(call_info));
        drawData();
    }

    void drawData() {
        voip_calls_info_t *call_info = data(0, Qt::UserRole).value<voip_calls_info_t*>();
        if (!call_info) {
            return;
        }
        char* addr_str;

        // XXX Pull digit count from capture file precision
        setText(start_time_col_, QString::number(nstime_to_sec(&(call_info->start_rel_ts)), 'f', 6));
        setText(stop_time_col_, QString::number(nstime_to_sec(&(call_info->stop_rel_ts)), 'f', 6));
        addr_str = (char*)address_to_display(NULL, &(call_info->initial_speaker));
        setText(initial_speaker_col_, addr_str);
        setText(from_col_, call_info->from_identity);
        setText(to_col_, call_info->to_identity);
        setText(protocol_col_, ((call_info->protocol == VOIP_COMMON) && call_info->protocol_name) ?
                        call_info->protocol_name : voip_protocol_name[call_info->protocol]);
        setText(packets_col_, QString::number(call_info->npackets));
        setText(state_col_, voip_call_state_name[call_info->call_state]);
        wmem_free(NULL, addr_str);

        /* Add comments based on the protocol */
        QString call_comments;
        switch (call_info->protocol) {
        case VOIP_ISUP:
        {
            isup_calls_info_t *isup_info = (isup_calls_info_t *)call_info->prot_info;
            call_comments = QString("%1-%2 %3 %4-%5")
                    .arg(isup_info->ni)
                    .arg(isup_info->opc)
                    .arg(UTF8_RIGHTWARDS_ARROW)
                    .arg(isup_info->ni)
                    .arg(isup_info->dpc);
        }
            break;
        case VOIP_H323:
        {
            h323_calls_info_t *h323_info = (h323_calls_info_t *)call_info->prot_info;
            gboolean flag = FALSE;
            static const QString on_str = QObject::tr("On");
            static const QString off_str = QObject::tr("Off");
            if (call_info->call_state == VOIP_CALL_SETUP) {
                flag = h323_info->is_faststart_Setup;
            } else {
                if ((h323_info->is_faststart_Setup) && (h323_info->is_faststart_Proc)) {
                    flag = TRUE;
                }
            }
            call_comments = QObject::tr("Tunneling: %1  Fast Start: %2")
                    .arg(h323_info->is_h245Tunneling ? on_str : off_str)
                    .arg(flag ? on_str : off_str);
        }
            break;
        case VOIP_COMMON:
        default:
            call_comments = call_info->call_comment;
            break;
        }
        setText(comments_col_, call_comments);
    }

    bool operator< (const QTreeWidgetItem &other) const
    {
        voip_calls_info_t *this_call_info = data(0, Qt::UserRole).value<voip_calls_info_t*>();
        voip_calls_info_t *other_call_info = other.data(0, Qt::UserRole).value<voip_calls_info_t*>();
        if (!this_call_info || !other_call_info) {
            return false;
        }

        switch (treeWidget()->sortColumn()) {
        case start_time_col_:
            return nstime_cmp(&(this_call_info->start_rel_ts), &(other_call_info->start_rel_ts)) < 0;
            break;
        case stop_time_col_:
            return nstime_cmp(&(this_call_info->stop_rel_ts), &(other_call_info->stop_rel_ts)) < 0;
            break;
        case initial_speaker_col_:
            return cmp_address(&(this_call_info->initial_speaker), &(other_call_info->initial_speaker)) < 0;
            break;
        case packets_col_:
            return this_call_info->npackets < other_call_info->npackets;
            break;
        default:
            break;
        }

        // Fall back to string comparison
        return QTreeWidgetItem::operator <(other);
    }

};

VoipCallsDialog::VoipCallsDialog(QWidget &parent, CaptureFile &cf, bool all_flows) :
    WiresharkDialog(parent, cf),
    ui(new Ui::VoipCallsDialog)
{
    ui->setupUi(this);
    ui->callTreeWidget->sortByColumn(start_time_col_, Qt::AscendingOrder);
    setWindowSubtitle(all_flows ? tr("SIP Flows") : tr("VoIP Calls"));

    ctx_menu_.addActions(QList<QAction *>() << ui->actionSelect_All);

    prepare_button_ = ui->buttonBox->addButton(tr("Prepare Filter"), QDialogButtonBox::ApplyRole);
    sequence_button_ = ui->buttonBox->addButton(tr("Flow Sequence"), QDialogButtonBox::ApplyRole);
    player_button_ = ui->buttonBox->addButton(tr("Play Call"), QDialogButtonBox::ApplyRole);
    player_button_->setIcon(StockIcon("media-playback-start"));

    // XXX Use recent settings instead
    resize(parent.width() * 4 / 5, parent.height() * 2 / 3);

    memset (&tapinfo_, 0, sizeof(tapinfo_));
    tapinfo_.tap_packet = tapPacket;
    tapinfo_.tap_draw = tapDraw;
    tapinfo_.tap_data = this;
    tapinfo_.callsinfos = g_queue_new();
    tapinfo_.h225_cstype = H225_OTHER;
    tapinfo_.fs_option = all_flows ? FLOW_ALL : FLOW_ONLY_INVITES; /* flow show option */
    tapinfo_.graph_analysis = sequence_analysis_info_new();
    tapinfo_.graph_analysis->type = SEQ_ANALYSIS_VOIP;

    voip_calls_init_all_taps(&tapinfo_);

    updateWidgets();

    tapinfo_.session = cap_file_.capFile()->epan;
    cap_file_.retapPackets();
}

VoipCallsDialog::~VoipCallsDialog()
{
    delete ui;

    voip_calls_remove_all_tap_listeners(&tapinfo_);
    sequence_analysis_info_free(tapinfo_.graph_analysis);
}

void VoipCallsDialog::captureFileClosing()
{
    voip_calls_remove_all_tap_listeners(&tapinfo_);
    tapinfo_.session = NULL;
    WiresharkDialog::captureFileClosing();
}

void VoipCallsDialog::contextMenuEvent(QContextMenuEvent *event)
{
    ctx_menu_.exec(event->globalPos());
}

void VoipCallsDialog::changeEvent(QEvent *event)
{
    if (0 != event)
    {
        switch (event->type())
        {
        case QEvent::LanguageChange:
            ui->retranslateUi(this);
            break;
        default:
            break;
        }
    }
    QDialog::changeEvent(event);
}

//void VoipCallsDialog::tapReset(void *tapinfo_ptr)
//{
//    Q_UNUSED(tapinfo_ptr)
//    voip_calls_tapinfo_t *tapinfo = (voip_calls_tapinfo_t *) tapinfo_ptr;
//}

gboolean VoipCallsDialog::tapPacket(void *tapinfo_ptr, packet_info *pinfo, epan_dissect_t *, const void *data)
{
    Q_UNUSED(tapinfo_ptr)
    Q_UNUSED(pinfo)
    Q_UNUSED(data)
#ifdef QT_MULTIMEDIAWIDGETS_LIB
//    voip_calls_tapinfo_t *tapinfo = (voip_calls_tapinfo_t *) tapinfo_ptr;
    // add_rtp_packet for voip player.
//    return TRUE;
#endif
    return FALSE;
}

void VoipCallsDialog::tapDraw(void *tapinfo_ptr)
{
    voip_calls_tapinfo_t *tapinfo = (voip_calls_tapinfo_t *) tapinfo_ptr;

    if (!tapinfo || !tapinfo->redraw) {
        return;
    }

    VoipCallsDialog *voip_calls_dialog = static_cast<VoipCallsDialog *>(tapinfo->tap_data);
    if (voip_calls_dialog) {
        voip_calls_dialog->updateCalls();
    }
}

void VoipCallsDialog::updateCalls()
{
    GList *cur_call = g_queue_peek_nth_link(tapinfo_.callsinfos, ui->callTreeWidget->topLevelItemCount());
    ui->callTreeWidget->setSortingEnabled(false);

    // Add any missing items
    while (cur_call && cur_call->data) {
        voip_calls_info_t *call_info = (voip_calls_info_t*) cur_call->data;
        new VoipCallsTreeWidgetItem(ui->callTreeWidget, call_info);
        cur_call = g_list_next(cur_call);
    }

    // Fill in the tree
    QTreeWidgetItemIterator iter(ui->callTreeWidget);
    while (*iter) {
        VoipCallsTreeWidgetItem *vcti = static_cast<VoipCallsTreeWidgetItem*>(*iter);
        vcti->drawData();
        ++iter;
    }

    // Resize columns
    for (int i = 0; i < ui->callTreeWidget->columnCount(); i++) {
        ui->callTreeWidget->resizeColumnToContents(i);
    }

    ui->callTreeWidget->setSortingEnabled(true);

    updateWidgets();
}

void VoipCallsDialog::updateWidgets()
{
    bool selected = ui->callTreeWidget->selectedItems().count() > 0 ? true : false;
    bool have_ga_items = false;

    if (tapinfo_.graph_analysis && tapinfo_.graph_analysis->items) {
        have_ga_items = true;
    }

    foreach (QMenu *submenu, ctx_menu_.findChildren<QMenu*>()) {
        submenu->setEnabled(selected);
    }
    prepare_button_->setEnabled(selected && have_ga_items);
    sequence_button_->setEnabled(selected && have_ga_items);
#if defined(QT_MULTIMEDIAWIDGETS_LIB) && 0 // We don't have a playback dialog yet.
    player_button_->setEnabled(selected && have_ga_items);
#else
    player_button_->setEnabled(false);
    player_button_->setText(tr("No Audio"));
#endif
}

void VoipCallsDialog::prepareFilter()
{
    if (ui->callTreeWidget->selectedItems().count() < 1 || !tapinfo_.graph_analysis) {
        return;
    }

    QString filter_str;
    QSet<guint16> selected_calls;

    /* Build a new filter based on frame numbers */
    const char *or_prepend = "";
    foreach (QTreeWidgetItem *ti, ui->callTreeWidget->selectedItems()) {
        voip_calls_info_t *call_info = ti->data(0, Qt::UserRole).value<voip_calls_info_t*>();
        selected_calls << call_info->call_num;
    }

    GList *cur_ga_item = g_queue_peek_nth_link(tapinfo_.graph_analysis->items, 0);
    while (cur_ga_item && cur_ga_item->data) {
        seq_analysis_item_t *ga_item = (seq_analysis_item_t*) cur_ga_item->data;
        if (selected_calls.contains(ga_item->conv_num)) {
            filter_str += QString("%1frame.number == %2").arg(or_prepend).arg(ga_item->fd->num);
            or_prepend = " or ";
        }
        cur_ga_item = g_list_next(cur_ga_item);
    }

#if 0
    // XXX The GTK+ UI falls back to building a filter based on protocols if the filter
    // length is too long. Leaving this here for the time being in case we need to do
    // the same in the Qt UI.
    const sip_calls_info_t *sipinfo;
    const isup_calls_info_t *isupinfo;
    const h323_calls_info_t *h323info;
    const h245_address_t *h245_add = NULL;
    const gcp_ctx_t* ctx;
    char* addr_str, *guid_str;

    if (filter_length < max_filter_length) {
        gtk_editable_insert_text(GTK_EDITABLE(main_display_filter_widget), filter_string_fwd->str, -1, &pos);
    } else {
        g_string_free(filter_string_fwd, TRUE);
        filter_string_fwd = g_string_new(filter_prepend);

        g_string_append_printf(filter_string_fwd, "(");
        is_first = TRUE;
        /* Build a new filter based on protocol fields */
        lista = g_queue_peek_nth_link(voip_calls_get_info()->callsinfos, 0);
        while (lista) {
            listinfo = (voip_calls_info_t *)lista->data;
            if (listinfo->selected) {
                if (!is_first)
                    g_string_append_printf(filter_string_fwd, " or ");
                switch (listinfo->protocol) {
                case VOIP_SIP:
                    sipinfo = (sip_calls_info_t *)listinfo->prot_info;
                    g_string_append_printf(filter_string_fwd,
                        "(sip.Call-ID == \"%s\")",
                        sipinfo->call_identifier
                    );
                    break;
                case VOIP_ISUP:
                    isupinfo = (isup_calls_info_t *)listinfo->prot_info;
                    g_string_append_printf(filter_string_fwd,
                        "(isup.cic == %i and frame.number >= %i and frame.number <= %i and mtp3.network_indicator == %i and ((mtp3.dpc == %i) and (mtp3.opc == %i)) or ((mtp3.dpc == %i) and (mtp3.opc == %i)))",
                        isupinfo->cic, listinfo->start_fd->num,
                        listinfo->stop_fd->num,
                        isupinfo->ni, isupinfo->dpc, isupinfo->opc,
                        isupinfo->opc, isupinfo->dpc
                    );
                    break;
                case VOIP_H323:
                    h323info = (h323_calls_info_t *)listinfo->prot_info;
					guid_str = guid_to_str(NULL, &h323info->guid[0]);
                    g_string_append_printf(filter_string_fwd,
                        "((h225.guid == %s || q931.call_ref == %x:%x || q931.call_ref == %x:%x)",
                        guid_str,
                        (guint8) (h323info->q931_crv & 0x00ff),
                        (guint8)((h323info->q931_crv & 0xff00)>>8),
                        (guint8) (h323info->q931_crv2 & 0x00ff),
                        (guint8)((h323info->q931_crv2 & 0xff00)>>8));
                    listb = g_list_first(h323info->h245_list);
					wmem_free(NULL, guid_str);
                    while (listb) {
                        h245_add = (h245_address_t *)listb->data;
                        addr_str = (char*)address_to_str(NULL, &h245_add->h245_address);
                        g_string_append_printf(filter_string_fwd,
                            " || (ip.addr == %s && tcp.port == %d && h245)",
                            addr_str, h245_add->h245_port);
                        listb = g_list_next(listb);
                        wmem_free(NULL, addr_str);
                    }
                    g_string_append_printf(filter_string_fwd, ")");
                    break;
                case TEL_H248:
                    ctx = (gcp_ctx_t *)listinfo->prot_info;
                    g_string_append_printf(filter_string_fwd,
                        "(h248.ctx == 0x%x)", ctx->id);
                    break;
                default:
                    /* placeholder to assure valid display filter expression */
                    g_string_append_printf(filter_string_fwd,
                        "(frame)");
                    break;
                }
                is_first = FALSE;
            }
            lista = g_list_next(lista);
        }

        g_string_append_printf(filter_string_fwd, ")");
        gtk_editable_insert_text(GTK_EDITABLE(main_display_filter_widget), filter_string_fwd->str, -1, &pos);
    }
#endif

    emit updateFilter(filter_str);
}

void VoipCallsDialog::showSequence()
{
    if (file_closed_) return;

    QSet<guint16> selected_calls;
    foreach (QTreeWidgetItem *ti, ui->callTreeWidget->selectedItems()) {
        voip_calls_info_t *call_info = ti->data(0, Qt::UserRole).value<voip_calls_info_t*>();
        selected_calls << call_info->call_num;
    }

    sequence_analysis_list_sort(tapinfo_.graph_analysis);
    GList *cur_ga_item = g_queue_peek_nth_link(tapinfo_.graph_analysis->items, 0);
    while (cur_ga_item && cur_ga_item->data) {
        seq_analysis_item_t *ga_item = (seq_analysis_item_t*) cur_ga_item->data;
        ga_item->display = selected_calls.contains(ga_item->conv_num);
        cur_ga_item = g_list_next(cur_ga_item);
    }

    SequenceDialog *sequence_dialog = new SequenceDialog(*parentWidget(), cap_file_, tapinfo_.graph_analysis);
    // XXX This goes away when we close the VoIP Calls dialog.
    connect(sequence_dialog, SIGNAL(goToPacket(int)),
            this, SIGNAL(goToPacket(int)));
    sequence_dialog->show();
}

void VoipCallsDialog::on_callTreeWidget_itemActivated(QTreeWidgetItem *item, int)
{
    voip_calls_info_t *call_info = item->data(0, Qt::UserRole).value<voip_calls_info_t*>();
    if (!call_info) {
        return;
    }
    emit goToPacket(call_info->start_fd->num);
}

void VoipCallsDialog::on_callTreeWidget_itemSelectionChanged()
{
    updateWidgets();
}

void VoipCallsDialog::on_actionSelect_All_triggered()
{
    ui->callTreeWidget->selectAll();
}

void VoipCallsDialog::on_buttonBox_clicked(QAbstractButton *button)
{
    if (button == prepare_button_) {
        prepareFilter();
    } else if (button == sequence_button_) {
        showSequence();
    }
}

void VoipCallsDialog::on_buttonBox_helpRequested()
{
    wsApp->helpTopicAction(HELP_TELEPHONY_VOIP_CALLS_DIALOG);
}

/*
 * Editor modelines
 *
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
