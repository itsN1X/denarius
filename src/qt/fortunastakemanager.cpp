#include "fortunastakemanager.h"
#include "ui_fortunastakemanager.h"
#include "addeditadrenalinenode.h"
#include "adrenalinenodeconfigdialog.h"

#include "sync.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "activefortunastake.h"
#include "fortunastakeconfig.h"
#include "fortunastake.h"
#include "walletdb.h"
#include "walletmodel.h"
#include "wallet.h"
#include "init.h"
#include "bitcoinrpc.h"
#include "askpassphrasedialog.h"

#include <boost/lexical_cast.hpp>
#include <fstream>
using namespace json_spirit;
using namespace std;

#include <QAbstractItemDelegate>
#include <QPainter>
#include <QTimer>
#include <QDebug>
#include <QScrollArea>
#include <QScroller>
#include <QDateTime>
#include <QApplication>
#include <QClipboard>
#include <QMessageBox>

FortunastakeManager::FortunastakeManager(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::FortunastakeManager),
    clientModel(0),
    walletModel(0)
{
    ui->setupUi(this);

    ui->editButton->setEnabled(false);
    ui->editButton->setVisible(false);
    ui->getConfigButton->setEnabled(false);
    ui->startButton->setEnabled(false);
    ui->stopButton->setEnabled(false);
    ui->copyAddressButton->setEnabled(false);

    ui->tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->tableWidget->setSortingEnabled(true);
    ui->tableWidget_2->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->tableWidget_2->setSortingEnabled(true);
    ui->tableWidget_2->sortByColumn(0, Qt::AscendingOrder);

    subscribeToCoreSignals();

    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(updateNodeList()));
    if(!GetBoolArg("-reindexaddr", false))
        timer->start(30000);

    QTimer::singleShot(1500, this, SLOT(updateNodeList()));
}

FortunastakeManager::~FortunastakeManager()
{
    delete ui;
}

class SortedWidgetItem : public QTableWidgetItem
{
public:
    bool operator <( const QTableWidgetItem& other ) const
    {
        return (data(Qt::UserRole) < other.data(Qt::UserRole));
    }
};

static void NotifyAdrenalineNodeUpdated(FortunastakeManager *page, CAdrenalineNodeConfig nodeConfig)
{
    // alias, address, privkey, collateral address
    QString alias = QString::fromStdString(nodeConfig.sAlias);
    QString addr = QString::fromStdString(nodeConfig.sAddress);
    QString privkey = QString::fromStdString(nodeConfig.sFortunastakePrivKey);
    
    QMetaObject::invokeMethod(page, "updateAdrenalineNode", Qt::QueuedConnection,
                              Q_ARG(QString, alias),
                              Q_ARG(QString, addr),
                              Q_ARG(QString, privkey)
                              );
}

void FortunastakeManager::subscribeToCoreSignals()
{
    // Connect signals to core
    uiInterface.NotifyAdrenalineNodeChanged.connect(boost::bind(&NotifyAdrenalineNodeUpdated, this, _1));
}

void FortunastakeManager::unsubscribeFromCoreSignals()
{
    // Disconnect signals from core
    uiInterface.NotifyAdrenalineNodeChanged.disconnect(boost::bind(&NotifyAdrenalineNodeUpdated, this, _1));
}

void FortunastakeManager::on_tableWidget_2_itemSelectionChanged()
{
    if(ui->tableWidget_2->selectedItems().count() > 0)
    {
        ui->editButton->setEnabled(true);
        ui->getConfigButton->setEnabled(true);
        ui->startButton->setEnabled(true);
        ui->stopButton->setEnabled(true);
        ui->copyAddressButton->setEnabled(true);
    }
}

void FortunastakeManager::updateAdrenalineNode(QString alias, QString addr, QString privkey)
{
    LOCK(cs_adrenaline);

    std::string errorMessage;
    QString status;
    QString collateral;
    QString payrate;

    uint256 mnTxHash;

    CTxDestination address1;
    CBitcoinAddress address2;
    int rank = 0;
    int outputIndex;
    TRY_LOCK(pwalletMain->cs_wallet, pwalletLock);

    if (!pwalletLock)
        return;

    BOOST_FOREACH(CFortunastakeConfig::CFortunastakeEntry mne, fortunastakeConfig.getEntries()) {
        if (mne.getAlias() == alias.toStdString())
        {
            mnTxHash.SetHex(mne.getTxHash());
            outputIndex = boost::lexical_cast<unsigned int>(mne.getOutputIndex());
            COutPoint outpoint = COutPoint(mnTxHash, outputIndex);
            if(pwalletMain->IsMine(CTxIn(outpoint)) != ISMINE_SPENDABLE) {
                if (fDebug) printf("FortunastakeManager:: %s %s - IS NOT SPENDABLE, status is bad!\n", mne.getTxHash().c_str(), mne.getOutputIndex().c_str());
                errorMessage = "Output is not spendable. ";
                break;
            }

            CWalletTx tx;
            if (pwalletMain->GetTransaction(mnTxHash, tx))
            {
                CTxOut vout = tx.vout[outputIndex];
                if (!ExtractDestination(vout.scriptPubKey, address1))
                    errorMessage += "Could not get collateral address. ";
                else
                    address2.Set(address1);
                if (vout.nValue != GetMNCollateral()*COIN)
                    errorMessage += "TX is not equal to 5000 D. ";
            }
            if (fDebug) printf("FortunastakeManager:: %s %s - found %s for alias %s\n", mne.getTxHash().c_str(), mne.getOutputIndex().c_str(), address2.ToString().c_str(),mne.getAlias().c_str());
            break;
        }
    }

    if (!address2.IsValid()) {
        errorMessage += "Could not find collateral address. ";
    }

    if (errorMessage == "" || mnCount > vecFortunastakes.size()) {
        status = QString::fromStdString("Loading");
        collateral = QString::fromStdString(address2.ToString().c_str());
    }
    else {
        status = QString::fromStdString("Error");
        collateral = QString::fromStdString(errorMessage);
    }

    BOOST_FOREACH(CFortunaStake& mn, vecFortunastakes) {
        if (mn.addr.ToString().c_str() == addr){
            rank = GetFortunastakeRank(mn, pindexBest);
            status = QString::fromStdString("Online");
            collateral = QString::fromStdString(address2.ToString().c_str());
            payrate = QString::fromStdString(strprintf("%.2f D", mn.payValue));
        }
    }

    if (vecFortunastakes.size() >= mnCount && rank == 0)
    {
        status = QString::fromStdString("Offline");
    }

    bool bFound = false;
    int nodeRow = 0;
    for(int i=0; i < ui->tableWidget_2->rowCount(); i++)
    {
        if(ui->tableWidget_2->item(i, 0)->text() == alias)
        {
            bFound = true;
            nodeRow = i;
            break;
        }
    }

    if(nodeRow == 0 && !bFound)
    {
        ui->tableWidget_2->insertRow(ui->tableWidget_2->rowCount());
        nodeRow = ui->tableWidget_2->rowCount()-1;
    }

    QTableWidgetItem *aliasItem = new QTableWidgetItem(alias);
    QTableWidgetItem *addrItem = new QTableWidgetItem(addr);
    QTableWidgetItem *statusItem = new QTableWidgetItem(status);
    QTableWidgetItem *collateralItem = new QTableWidgetItem(collateral);
    SortedWidgetItem *rankItem = new SortedWidgetItem();
    SortedWidgetItem *payrateItem = new QTableWidgetItem(payrate);

    rankItem->setData(Qt::UserRole, rank ? rank : 2000);
    rankItem->setData(Qt::DisplayRole, rank > 0 && rank < 500000 ? QString::number(rank) : "");

    ui->tableWidget_2->setItem(nodeRow, 0, aliasItem);
    ui->tableWidget_2->setItem(nodeRow, 1, addrItem);
    ui->tableWidget_2->setItem(nodeRow, 2, rankItem);
    ui->tableWidget_2->setItem(nodeRow, 3, statusItem);
    ui->tableWidget_2->setItem(nodeRow, 4, payrateItem);
    ui->tableWidget_2->setItem(nodeRow, 5, collateralItem);
}

static QString seconds_to_DHMS(quint32 duration)
{
  QString res;
  int seconds = (int) (duration % 60);
  duration /= 60;
  int minutes = (int) (duration % 60);
  duration /= 60;
  int hours = (int) (duration % 24);
  int days = (int) (duration / 24);
  if((hours == 0)&&(days == 0))
      return res.sprintf("%02dm:%02ds", minutes, seconds);
  if (days == 0)
      return res.sprintf("%02dh:%02dm:%02ds", hours, minutes, seconds);
  return res.sprintf("%dd %02dh:%02dm:%02ds", days, hours, minutes, seconds);
}

void FortunastakeManager::updateNodeList()
{
    TRY_LOCK(cs_fortunastakes, lockFortunastakes);
    if(!lockFortunastakes)
        return;

    ui->countLabel->setText("Updating...");
    if (mnCount == 0 || IsInitialBlockDownload()) return;

    ui->tableWidget->clearContents();
    ui->tableWidget->setRowCount(0);
    ui->tableWidget->setSortingEnabled(false);

    BOOST_FOREACH(CFortunaStake mn, vecFortunastakes) 
    {
        int mnRow = 0;
        ui->tableWidget->insertRow(0);
        int mnRank = GetFortunastakeRank(mn, pindexBest);
        int64_t value;
        double rate;
        mn.GetPaymentInfo(pindexBest, value, rate);
        QString payrate = QString::fromStdString(strprintf("%sD", FormatMoney(value).c_str()));
        // populate list
        // Address, Rank, Active, Active Seconds, Last Seen, Pub Key
        QTableWidgetItem *activeItem = new QTableWidgetItem();
        activeItem->setData(Qt::DisplayRole, QString::fromStdString(mn.IsEnabled() ? "Y" : "N"));
        QTableWidgetItem *addressItem = new QTableWidgetItem();
        addressItem->setData(Qt::EditRole, QString::fromStdString(mn.addr.ToString()));
        SortedWidgetItem *rankItem = new SortedWidgetItem();
        rankItem->setData(Qt::UserRole, mnRank);
        rankItem->setData(Qt::DisplayRole, QString("%2 (%1)").arg(QString::number(mnRank)).arg(payrate));
        SortedWidgetItem *activeSecondsItem = new SortedWidgetItem();
        activeSecondsItem->setData(Qt::UserRole, (qint64)(mn.lastTimeSeen - mn.now));
        activeSecondsItem->setData(Qt::DisplayRole, seconds_to_DHMS((qint64)(mn.lastTimeSeen - mn.now)));
        SortedWidgetItem *lastSeenItem = new SortedWidgetItem();
        lastSeenItem->setData(Qt::UserRole, (qint64)mn.lastTimeSeen);
        lastSeenItem->setData(Qt::DisplayRole, QString::fromStdString(DateTimeStrFormat(mn.lastTimeSeen)));

        CScript pubkey;
        pubkey =GetScriptForDestination(mn.pubkey.GetID());
        CTxDestination address1;
        ExtractDestination(pubkey, address1);
        CBitcoinAddress address2(address1);
        QTableWidgetItem *pubkeyItem = new QTableWidgetItem(QString::fromStdString(address2.ToString())); // +strprintf(" - %.2fD (%3.0f%%)", FormatMoney(value).c_str(), rate)

        ui->tableWidget->setItem(mnRow, 0, addressItem);
        ui->tableWidget->setItem(mnRow, 1, rankItem);
        ui->tableWidget->setItem(mnRow, 2, activeItem);
        ui->tableWidget->setItem(mnRow, 3, activeSecondsItem);
        ui->tableWidget->setItem(mnRow, 4, lastSeenItem);
        ui->tableWidget->setItem(mnRow, 5, pubkeyItem);
    }

    if (mnCount > 0)
        ui->countLabel->setText(QString("%1/%2 active (average income: %3)").arg(vecFortunastakes.size()).arg(mnCount).arg(QString::fromStdString(FormatMoney(nAverageFSIncome))));
    else
        ui->countLabel->setText("Loading...");

    if (mnCount < vecFortunastakes.size())
        ui->countLabel->setText(QString("%1 active (average income: %2)").arg(vecFortunastakes.size()).arg(QString::fromStdString(FormatMoney(nAverageFSIncome))));

    ui->tableWidget->setSortingEnabled(true);

    if(pwalletMain)
    {
        LOCK(cs_adrenaline);
        BOOST_FOREACH(PAIRTYPE(std::string, CAdrenalineNodeConfig) adrenaline, pwalletMain->mapMyAdrenalineNodes)
        {
            updateAdrenalineNode(QString::fromStdString(adrenaline.second.sAlias), QString::fromStdString(adrenaline.second.sAddress), QString::fromStdString(adrenaline.second.sFortunastakePrivKey));
        }
    }
    
}


void FortunastakeManager::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if(model)
    {
    }
}

void FortunastakeManager::setWalletModel(WalletModel *model)
{
    this->walletModel = model;

}

void FortunastakeManager::on_createButton_clicked()
{
    AddEditAdrenalineNode* aenode = new AddEditAdrenalineNode();
    aenode->exec();
}

void FortunastakeManager::on_copyAddressButton_clicked()
{
    QItemSelectionModel* selectionModel = ui->tableWidget_2->selectionModel();
    QModelIndexList selected = selectionModel->selectedRows();
    if(selected.count() == 0)
        return;

    QModelIndex index = selected.at(0);
    int r = index.row();
    std::string sCollateralAddress = ui->tableWidget_2->item(r, 3)->text().toStdString();

    QApplication::clipboard()->setText(QString::fromStdString(sCollateralAddress));
}

void FortunastakeManager::on_editButton_clicked()
{
    QItemSelectionModel* selectionModel = ui->tableWidget_2->selectionModel();
    QModelIndexList selected = selectionModel->selectedRows();
    if(selected.count() == 0)
        return;

    QModelIndex index = selected.at(0);
    int r = index.row();
    std::string sAddress = ui->tableWidget_2->item(r, 1)->text().toStdString();

    // get existing config entry

}

void FortunastakeManager::on_getConfigButton_clicked()
{
    QItemSelectionModel* selectionModel = ui->tableWidget_2->selectionModel();
    QModelIndexList selected = selectionModel->selectedRows();
    if(selected.count() == 0)
        return;

    QModelIndex index = selected.at(0);
    int r = index.row();
    std::string sAddress = ui->tableWidget_2->item(r, 1)->text().toStdString();
    CAdrenalineNodeConfig c = pwalletMain->mapMyAdrenalineNodes[sAddress];
    std::string sPrivKey = c.sFortunastakePrivKey;
    AdrenalineNodeConfigDialog* d = new AdrenalineNodeConfigDialog(this, QString::fromStdString(sAddress), QString::fromStdString(sPrivKey));
    d->exec();
}

void FortunastakeManager::on_startButton_clicked()
{
    // start the node
    QItemSelectionModel* selectionModel = ui->tableWidget_2->selectionModel();
    QModelIndexList selected = selectionModel->selectedRows();
    if(selected.count() == 0)
        return;

    QModelIndex index = selected.at(0);
    int r = index.row();
    std::string sAddress = ui->tableWidget_2->item(r, 1)->text().toStdString();
    CAdrenalineNodeConfig c = pwalletMain->mapMyAdrenalineNodes[sAddress];

    std::string errorMessage;
    bool result = activeFortunastake.Register(c.sAddress, c.sFortunastakePrivKey, c.sTxHash, c.sOutputIndex, errorMessage);

    QMessageBox msg;
    msg.setWindowTitle("Denarius Message");
    if(result)
        msg.setText("Hybrid Fortunastake at " + QString::fromStdString(c.sAddress) + " started.");
    else
        msg.setText("Error: " + QString::fromStdString(errorMessage));

    msg.exec();
}

void FortunastakeManager::on_startAllButton_clicked()
{
    QString results;
    WalletModel::UnlockContext ctx(walletModel->requestUnlock());

    if(!ctx.isValid())
    {
        results = "Wallet failed to unlock.\n";
    } else {
        std::vector<CFortunastakeConfig::CFortunastakeEntry> mnEntries;
        mnEntries = fortunastakeConfig.getEntries();

        int total = 0;
        int successful = 0;
        int fail = 0;

        BOOST_FOREACH(CFortunastakeConfig::CFortunastakeEntry mne, fortunastakeConfig.getEntries()) {
            total++;

            std::string errorMessage;
            bool result = activeFortunastake.Register(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), errorMessage);

            if(result)
            {
                results.append(QString("%1 (%2): OK\n").arg(QString::fromStdString(mne.getIp())).arg(QString::fromStdString(mne.getAlias())));
                updateAdrenalineNode(QString::fromStdString(mne.getAlias()),QString::fromStdString(mne.getIp()),QString::fromStdString(mne.getPrivKey()));
                successful++;
            }
            else
            {
                results.append(QString("%1 (%2): %3\n").arg(QString::fromStdString(mne.getIp())).arg(QString::fromStdString(mne.getAlias())).arg(QString::fromStdString(errorMessage)));
                fail++;
            }
        }

        results += QString::fromStdString("Successfully started " + boost::lexical_cast<std::string>(successful) + " fortunastakes, failed to start " +
                boost::lexical_cast<std::string>(fail) + ", total " + boost::lexical_cast<std::string>(total));
    }

    if(ctx.isValid())
    {
        pwalletMain->Lock();
    }

    QMessageBox msg;
    msg.setWindowTitle("Denarius Message");
    msg.setText(results);
    msg.exec();

}

void FortunastakeManager::on_removeButton_clicked()
{
    QItemSelectionModel* selectionModel = ui->tableWidget_2->selectionModel();
    QModelIndexList selected = selectionModel->selectedRows();
    if(selected.count() == 0)
        return;

    QMessageBox::StandardButton confirm;
    confirm = QMessageBox::question(this, "Delete fortunastake.conf?", "Are you sure you want to delete this hybrid fortunastake configuration?", QMessageBox::Yes|QMessageBox::No);

    if(confirm == QMessageBox::Yes)
    {
        QModelIndex index = selected.at(0);
        int r = index.row();
        std::string sAddress = ui->tableWidget_2->item(r, 1)->text().toStdString();

        CAdrenalineNodeConfig c = pwalletMain->mapMyAdrenalineNodes[sAddress];
        CWalletDB walletdb(pwalletMain->strWalletFile);

        BOOST_FOREACH(CFortunastakeConfig::CFortunastakeEntry mne, fortunastakeConfig.getEntries())
        {
            if (mne.getIp() == sAddress)
            {
                fortunastakeConfig.purge(mne);
                std::map<std::string, CAdrenalineNodeConfig>::iterator iter = pwalletMain->mapMyAdrenalineNodes.find(sAddress);
                if (iter != pwalletMain->mapMyAdrenalineNodes.end())
                    pwalletMain->mapMyAdrenalineNodes.erase(iter);
                walletdb.EraseAdrenalineNodeConfig(sAddress);

                ui->tableWidget_2->removeRow(r);
                break;
            }
        }
    }
}

void FortunastakeManager::on_stopButton_clicked()
{
    // start the node
    QItemSelectionModel* selectionModel = ui->tableWidget_2->selectionModel();
    QModelIndexList selected = selectionModel->selectedRows();
    if(selected.count() == 0)
        return;

    QModelIndex index = selected.at(0);
    int r = index.row();
    std::string sAddress = ui->tableWidget_2->item(r, 1)->text().toStdString();
    CAdrenalineNodeConfig c = pwalletMain->mapMyAdrenalineNodes[sAddress];

    std::string errorMessage;
    bool result = activeFortunastake.StopFortunaStake(c.sAddress, c.sFortunastakePrivKey, errorMessage);
    QMessageBox msg;
    msg.setWindowTitle("Denarius Message");
    if(result)
    {
        msg.setText("Hybrid Fortunastake at " + QString::fromStdString(c.sAddress) + " stopped.");
    }
    else
    {
        msg.setText("Error: " + QString::fromStdString(errorMessage));
    }
    msg.exec();
}

void FortunastakeManager::on_stopAllButton_clicked()
{
    std::string results;
    BOOST_FOREACH(PAIRTYPE(std::string, CAdrenalineNodeConfig) adrenaline, pwalletMain->mapMyAdrenalineNodes)
    {
        CAdrenalineNodeConfig c = adrenaline.second;
	std::string errorMessage;
        bool result = activeFortunastake.StopFortunaStake(c.sAddress, c.sFortunastakePrivKey, errorMessage);
	if(result)
	{
   	    results += c.sAddress + ": STOPPED\n";
	}	
	else
	{
	    results += c.sAddress + ": ERROR: " + errorMessage + "\n";
	}
    }

    QMessageBox msg;
    msg.setWindowTitle("Denarius Message");
    msg.setText(QString::fromStdString(results));
    msg.exec();
}
