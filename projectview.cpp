#include "projectview.h"
#include "ui_projectview.h"

#include <QFileDialog>
#include <QFileSystemModel>
#include <QStringListModel>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QProcess>
#include <QMessageBox>
#include <QDir>
#include <QFuture>

#include <QCheckBox>
#include <QInputDialog>
#include <QProgressBar>
#include <QtConcurrent>
#include <QtDebug>

#include "projecticonprovider.h"
#include "targetupdatediscover.h"
#include "etags.h"

ProjectView::ProjectView(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::DocumentView)
{
    buildProc = new QProcess(this);
    buildProc->setObjectName("buildProc");
    connect(buildProc, SIGNAL(finished(int)), this, SIGNAL(buildEnd(int)));
    ui->setupUi(this);
    QFile filterFiles(":/build/project-filters.txt");
    filterFiles.open(QFile::ReadOnly);
    while (!filterFiles.atEnd()) {
        QRegExp crlf(R"([\r\n]*)");
        QString name = QString(filterFiles.readLine()).remove(':').remove(crlf);
        QString filter = QString(filterFiles.readLine()).remove(crlf);
        // qDebug() << "filter" << name << filter;
        if (!name.isEmpty() && !filter.isEmpty())
            ui->filterCombo->addItem(name, filter.split(' '));
        else
            break;
    }
    ui->tabWidget->setCurrentIndex(0);
}

ProjectView::~ProjectView()
{
    delete ui;
}

QString ProjectView::project() const
{
    QFileSystemModel *m = qobject_cast<QFileSystemModel*>(ui->treeView->model());
    if (!m)
        return QString();
    return m->rootDirectory().absoluteFilePath("Makefile");
}

QDir ProjectView::projectPath() const
{
    QFileSystemModel *m = qobject_cast<QFileSystemModel*>(ui->treeView->model());
    if (!m)
        return QDir();
    return m->rootDirectory();
}

void ProjectView::closeProject()
{
    if (ui->treeView->model()) {
        ui->treeView->model()->deleteLater();
        ui->treeView->setModel(0l);
    }
    ui->targetList->clear();
}

void ProjectView::openProject(const QString &projectFile)
{
    if (!project().isEmpty())
        closeProject();
    if (!projectFile.isEmpty()) {
        QFileInfo mk(projectFile);
        QFileSystemModel *model = new QFileSystemModel(this);
        model->setFilter(QDir::AllDirs|QDir::NoDotAndDotDot|QDir::Files);
        model->setNameFilterDisables(false);
        model->setNameFilters(QStringList("*"));
        model->setIconProvider(new ProjectIconProvider(this));
        ui->treeView->model()->deleteLater();
        ui->treeView->setModel(model);
        ui->treeView->setRootIndex(model->setRootPath(mk.path()));
        for(int i=2; i<model->columnCount(); i++)
            ui->treeView->hideColumn(i);
        ui->treeView->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
        TargetUpdateDiscover *discover = new TargetUpdateDiscover(this);
        connect(discover, SIGNAL(updateFinish(MakefileInfo)), this, SLOT(updateMakefileInfo(MakefileInfo)));
        discover->start(project());
    }
}

void ProjectView::buildStart(const QString &target)
{
    if (buildProc->state() != QProcess::NotRunning)
        buildStop();
    buildProc->setWorkingDirectory(QFileInfo(project()).absoluteDir().absolutePath());
    buildProc->start(QString("make -f \"%1\" \"%2\"")
                     .arg(project())
                     .arg(target));
}

void ProjectView::buildStop()
{
    if (buildProc->state() != QProcess::NotRunning) {
        buildProc->terminate();
        if (!buildProc->waitForFinished(1000)) {
            emit buildStderr(tr("Killing process"));
            buildProc->kill();
        }
    }
}

void ProjectView::setDebugOn(bool on)
{
    if (on) {
        ui->tabWidget->setCurrentWidget(ui->tabDebug);
    } else {
        ui->tabWidget->setCurrentWidget(ui->tabProject);
    }
}

void ProjectView::refreshTags()
{
    qDebug() << "parse tags " << mk_info.tags.parse(QDir(mk_info.workingDir).absoluteFilePath("TAGS"));
}

void ProjectView::on_treeView_activated(const QModelIndex &index)
{
    QFileSystemModel *m = qobject_cast<QFileSystemModel*>(ui->treeView->model());
    if (m) {
        QFileInfo f = m->fileInfo(index);
        if (f.isFile())
            emit fileOpen(f.filePath());
    }
}

void ProjectView::updateMakefileInfo(const MakefileInfo &info)
{
    mk_info = info;
    QIcon icon = QIcon::fromTheme("run-build-configure", QIcon(":/icon-theme/icon-theme/run-build-configure.png"));
    ui->targetList->clear();
    QStringList orderedTargets = mk_info.targets;
    orderedTargets.sort();
    foreach(QString t, orderedTargets)
        ui->targetList->addItem(new QListWidgetItem(icon, t));
    sender()->deleteLater();
    QProcess *ctagProc = new QProcess(this);
    ctagProc->setWorkingDirectory(mk_info.workingDir);
    connect(ctagProc, static_cast<void (QProcess::*)(int)>(&QProcess::finished),
            [ctagProc, this] () {
        this->refreshTags();
        ctagProc->deleteLater();
    });
    ctagProc->start("ctags -R -e --c-kinds=+p --c++-kinds=+px");
    emit projectOpened();
}

void ProjectView::on_targetList_doubleClicked(const QModelIndex &index)
{
    QListWidgetItem *item = ui->targetList->item(index.row());
    emit startBuild(item->text());
}

void ProjectView::on_buildProc_readyReadStandardError()
{
    emit buildStderr(buildProc->readAllStandardError());
}

void ProjectView::on_buildProc_readyReadStandardOutput()
{
    emit buildStdout(buildProc->readAllStandardOutput());
}

void ProjectView::on_filterCombo_activated(int idx)
{
    QFileSystemModel *m = qobject_cast<QFileSystemModel*>(ui->treeView->model());
    if (m)
        m->setNameFilters(ui->filterCombo->itemData(idx).toStringList());
}

void ProjectView::on_filterButton_clicked()
{
    // TODO add files filter selection
}

void ProjectView::on_toolButton_documentNew_clicked()
{
    if (!ui->treeView->selectionModel())
        return;
    QFileSystemModel *m = qobject_cast<QFileSystemModel*>(ui->treeView->model());
    if (!m)
        return;
    QModelIndex idx = ui->treeView->selectionModel()->selectedIndexes().isEmpty()?
                ui->treeView->rootIndex() :
                ui->treeView->selectionModel()->selectedIndexes().first();
    QFileInfo info = m->fileInfo(idx);
    if (!info.isDir()) {
        info = QFileInfo(info.absoluteDir().absolutePath());
    }
    QString fileName = QInputDialog::getText(this->parentWidget(), tr("File name"),
                                             tr("Create file on %1")
                                                .arg(m->fileInfo(idx).absoluteFilePath()));
    if (!fileName.isEmpty()) {
        QFile f(QDir(info.absoluteFilePath()).absoluteFilePath(fileName));
        if (!f.open(QFile::WriteOnly)) {
            QMessageBox::critical(this, tr("Error creating file"), f.errorString());
        } else {
            f.close();
            fileOpen(fileName);
        }
    }
}

void ProjectView::on_toolButton_folderNew_clicked()
{
    if (!ui->treeView->selectionModel())
        return;
    QFileSystemModel *m = qobject_cast<QFileSystemModel*>(ui->treeView->model());
    if (!m)
        return;
    QModelIndex idx = ui->treeView->selectionModel()->selectedIndexes().isEmpty()?
                ui->treeView->rootIndex():
                ui->treeView->selectionModel()->selectedIndexes().first();
    if (!QFileInfo(m->fileInfo(idx)).isDir()) {
        idx = idx.parent();
        if (!m->fileInfo(idx).isDir()) {
            qDebug() << "ERROR parent not a dir";
            return;
        }
    }
    QString name = QInputDialog::getText(this->parentWidget(), tr("Folder name"),
                                         tr("Create folder on %1")
                                            .arg(m->fileInfo(idx).absoluteFilePath()));
    if (!name.isEmpty()) {
        qDebug() << "creating" << name << " on " << m->fileName(idx);
        m->mkdir(idx, name);
    }
}

void ProjectView::on_toolButton_elementDel_clicked()
{
    if (!ui->treeView->selectionModel())
        return;
    QFileSystemModel *m = qobject_cast<QFileSystemModel*>(ui->treeView->model());
    if (!m)
        return;
    QModelIndexList items = ui->treeView->selectionModel()->selectedRows(0);
    QMessageBox msg(window());
    msg.setWindowTitle(tr("Delete files"));
    msg.setIcon(QMessageBox::Warning);
    msg.addButton(QMessageBox::Yes);
    msg.addButton(QMessageBox::No);
    if (items.count() > 1) {
        QCheckBox *forAll = new QCheckBox(tr("Do this operation for all items"), &msg);
        forAll->setCheckState(Qt::Unchecked);
        msg.setCheckBox(forAll);
        msg.addButton(QMessageBox::Cancel);
    }
    int last = -1;
    foreach(QModelIndex idx, items) {
        QModelIndex parent = idx.parent();
        QString name = m->filePath(idx);
        bool doForAll = false;
        if (msg.checkBox())
            doForAll = (msg.checkBox()->checkState() == Qt::Checked);
        if (!doForAll) {
            msg.setText(tr("Realy remove %1").arg(name));
            last = msg.exec();
        }
        switch(last) {
        case QMessageBox::Yes:
            if (m->fileInfo(idx).isDir()) {
                QDir(m->fileInfo(idx).absoluteFilePath()).removeRecursively();
            } else {
                m->remove(idx);
            }
            ui->treeView->update(parent);
            break;
        case QMessageBox::No:
            break;
        case QMessageBox::Cancel:
            return;
        }
    }
}
