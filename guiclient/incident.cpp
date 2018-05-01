/*
 * This file is part of the xTuple ERP: PostBooks Edition, a free and
 * open source Enterprise Resource Planning software suite,
 * Copyright (c) 1999-2018 by OpenMFG LLC, d/b/a xTuple.
 * It is licensed to you under the Common Public Attribution License
 * version 1.0, the full text of which (including xTuple-specific Exhibits)
 * is available at www.xtuple.com/CPAL.  By using this software, you agree
 * to be bound by its terms.
 */

#include "incident.h"

#include <QAction>
#include <QMenu>
#include <QMessageBox>
#include <QSqlError>
#include <QVariant>

#include "arOpenItem.h"
#include "errorReporter.h"
#include "returnAuthorization.h"
#include "storedProcErrorLookup.h"
#include "incidentHistory.h"
#include "task.h"
#include "guiErrorCheck.h"
#include "parameterwidget.h"

#include <openreports.h>

bool incident::userHasPriv(const int pMode, const int pId)
{
  if (_privileges->check("MaintainAllIncidents"))
    return true;
  bool personalPriv = _privileges->check("MaintainPersonalIncidents");
  if(pMode==cView)
  {
    if(_privileges->check("ViewAllIncidents"))
      return true;
    personalPriv = personalPriv || _privileges->check("ViewPersonalIncidents");
  }

  if(pMode==cNew)
    return personalPriv;
  else
  {
    XSqlQuery usernameCheck;
    usernameCheck.prepare( "SELECT getEffectiveXtUser() IN (incdt_owner_username, incdt_assigned_username) AS canModify "
                           "FROM incdt "
                            "WHERE (incdt_id=:incdt_id);" );
    usernameCheck.bindValue(":incdt_id", pId);
    usernameCheck.exec();

    if (usernameCheck.first())
      return usernameCheck.value("canModify").toBool()&&personalPriv;
    return false;
  }
}

incident::incident(QWidget* parent, const char* name, bool modal, Qt::WindowFlags fl)
    : XDialog(parent, name, modal, fl)
{
  _statusCodes << "N" << "F" << "C" << "A" << "R" << "L";
  setupUi(this);

  _incdtid = -1;
  _saved = false;
  _aropenid = -1;
  _close = false;

  _print = _buttonBox->addButton(tr("Print"),QDialogButtonBox::ActionRole);
  _print->setObjectName("_print");
  _buttonBox->button(QDialogButtonBox::Save)->setObjectName("_save");

  if(!_privileges->check("EditOwner")) _owner->setEnabled(false);

  connect(_assignedTo,    SIGNAL(newId(int)),       this,       SLOT(sAssigned()));
  connect(_buttonBox,     SIGNAL(rejected()),       this,       SLOT(sCancel()));
  connect(_crmacct,       SIGNAL(newId(int)),       this,       SLOT(sCRMAcctChanged(int)));
  connect(_item,          SIGNAL(newId(int)),     _lotserial,   SLOT(setItemId(int)));
  connect(_incdthist,     SIGNAL(itemSelected(int)), this,      SLOT(sIncidentHistory()));
  connect(_buttonBox,     SIGNAL(accepted()),       this,       SLOT(sSave()));
  connect(_print,         SIGNAL(clicked()),        this,       SLOT(sPrint()));
  connect(_viewAR,        SIGNAL(clicked()),        this,       SLOT(sViewAR()));
  connect(_project,       SIGNAL(newId(int)),       this,       SLOT(sProjectUpdated()));

  _charass->setType("INCDT");
  _severity->setType(XComboBox::IncidentSeverity);
  _priority->setType(XComboBox::IncidentPriority);
  _resolution->setType(XComboBox::IncidentResolution);
  _category->setType(XComboBox::IncidentCategory);
  _lotserial->setStrict(false);

  _incdthist->addColumn(tr("Username"),     _userColumn, Qt::AlignLeft, true, "incdthist_username");
  _incdthist->addColumn(tr("Date/Time"),_timeDateColumn, Qt::AlignLeft, true, "incdthist_timestamp");
  _incdthist->addColumn(tr("Description"),           -1, Qt::AlignLeft, true, "incdthist_descrip");

  _taskList = new taskList(this, "taskList", Qt::Widget);
  _taskListTab->layout()->addWidget(_taskList);
  _taskList->setCloseVisible(false);
  _taskList->list()->hideColumn("crmacct_number");
  _taskList->list()->hideColumn("crmacct_name");
  _taskList->list()->hideColumn("parent");
  _taskList->parameterWidget()->setDefault(tr("User"), QVariant(), true);
  _taskList->parameterWidget()->append("hasContext", "hasContext", ParameterWidget::Exists, true);
  _taskList->setParameterWidgetVisible(false);
  _taskList->setQueryOnStartEnabled(false);
  _taskList->_opportunities->setForgetful(true);
  _taskList->_opportunities->setChecked(false);
  _taskList->_incidents->setForgetful(true);
  _taskList->_incidents->setChecked(false);
  _taskList->_projects->setForgetful(true);
  _taskList->_projects->setChecked(false);
  _taskList->_showGroup->setVisible(false);
  _taskList->_showCompleted->setVisible(true);
  _taskList->setParent("INCDT");

  _owner->setUsername(omfgThis->username());
  _owner->setType(UsernameLineEdit::UsersActive);
  _assignedTo->setType(UsernameLineEdit::UsersActive);

  if (_metrics->boolean("LotSerialControl"))
  {
    connect(_item, SIGNAL(valid(bool)), _lotserial, SLOT(setEnabled(bool)));
    connect(_item, SIGNAL(newId(int)),  _lotserial, SLOT(setItemId(int)));
  }
  else
    _lotserial->setVisible(false);

  if(!_metrics->boolean("IncidentsPublicPrivate"))
    _public->hide();
  _public->setChecked(_metrics->boolean("IncidentPublicDefault"));

  // because this causes a pop-behind situation we are hiding for now.
  //_return->hide();
}

incident::~incident()
{
  // no need to delete child widgets, Qt does it all for us
}

void incident::languageChange()
{
  retranslateUi(this);
}

enum SetResponse incident::set(const ParameterList &pParams)
{
  XSqlQuery incidentet;
  XDialog::set(pParams);
  QVariant param;
  bool     valid;

  param = pParams.value("incdt_id", &valid);
  if (valid)
  {
    _incdtid = param.toInt();
    populate();
    _lotserial->setItemId(_item->id());
    _charass->setId(_incdtid);
  }

  param = pParams.value("mode", &valid);
  if (valid)
  {
    _mode = cNew;

    if (param.toString() == "new")
    {
      incidentet.exec("SELECT nextval('incdt_incdt_id_seq') AS incdt_id, "
           "fetchIncidentNumber() AS number,"
           "COALESCE((SELECT incdtpriority_id FROM incdtpriority "
           "          WHERE incdtpriority_default), -1) AS prioritydefault;");
      if(incidentet.first())
      {
        _incdtid=incidentet.value("incdt_id").toInt();
        _number->setText(incidentet.value("number").toString());
        _comments->setId(_incdtid);
        _documents->setId(_incdtid);
        _charass->setId(_incdtid);
        _alarms->setId(_incdtid);
        _recurring->setParent(_incdtid, "INCDT");
        _priority->setId(incidentet.value("prioritydefault").toInt());
        _print->hide();
        _project->setAllowedStatuses(ProjectLineEdit::Concept |  ProjectLineEdit::InProcess);
      }
      else
      {
        QMessageBox::critical( omfgThis, tr("Database Error"),
                               tr( "A Database Error occured in incident::New:"
                                   "\n%1" ).arg(incidentet.lastError().text()));
        reject();
      }
      // Characteristics update incident history so we have to save the incident first
      // when adding a characteristic in new mode otherwise we get foreign key errors
      QPushButton *newbutton = _charass->findChild<QPushButton*>("_newCharacteristic");
      disconnect(newbutton, SIGNAL(clicked()), _charass, SLOT(sNew()));
      connect(newbutton, SIGNAL(clicked()), this, SLOT(sNewCharacteristic()));
    }
    else if (param.toString() == "edit")
    {
      _mode = cEdit;

      _crmacct->setEnabled(true);
    }
    else if (param.toString() == "view")
      setViewMode();
  }

  _taskList->parameterWidget()->setDefault(tr("Incident"), _incdtid, true);
  _taskList->sFillList();

  param = pParams.value("crmacct_id", &valid);
  if (valid)
  {
    _crmacct->setId(param.toInt());
    _crmacct->setEnabled(false);
  }

  param = pParams.value("cntct_id", &valid);
  if (valid)
  {
    _cntct->setId(param.toInt());
  }

  param = pParams.value("prj_id", &valid);
  if (valid)
  {
    _project->setId(param.toInt());
  }
  
  param = pParams.value("aropen_id", &valid);
  if (valid)
  {
    _aropenid = param.toInt();
    incidentet.prepare("SELECT aropen_doctype, aropen_docnumber, "
              "       CASE WHEN (aropen_doctype='C') THEN :creditMemo"
              "            WHEN (aropen_doctype='D') THEN :debitMemo"
              "            WHEN (aropen_doctype='I') THEN :invoice"
              "            WHEN (aropen_doctype='R') THEN :cashdeposit"
              "            ELSE '' END AS docType "
              "FROM aropen "
              "WHERE (aropen_id=:aropen_id);");
    incidentet.bindValue(":aropen_id", _aropenid);
    incidentet.bindValue(":creditMemo", tr("Credit Memo"));
    incidentet.bindValue(":debitMemo", tr("Debit Memo"));
    incidentet.bindValue(":invoice", tr("Invoice"));
    incidentet.bindValue(":cashdeposit", tr("Customer Deposit"));
    incidentet.exec();
    if (incidentet.first())
    {
      if (_metrics->value("DefaultARIncidentStatus").toInt())
        _category->setId(_metrics->value("DefaultARIncidentStatus").toInt());
      _ardoctype=incidentet.value("aropen_doctype").toString();
      _docType->setText(incidentet.value("docType").toString());
      _docNumber->setText(incidentet.value("aropen_docnumber").toString());
      _description->setText(QString("%1 #%2").arg(incidentet.value("docType").toString()).arg(incidentet.value("aropen_docnumber").toString()));
    }
  }

  connect(_category,      SIGNAL(newID(int)),       this,       SLOT(sIncdtCategoryChanged(int)));
  return NoError;
}

void incident::setViewMode()
{
  _mode = cView;

  _crmacct->setEnabled(false);
  _cntct->setEnabled(false);
  _assignedTo->setEnabled(false);
  _category->setEnabled(false);
  _status->setEnabled(false);
  _resolution->setEnabled(false);
  _severity->setEnabled(false);
  _priority->setEnabled(false);
  _item->setReadOnly(true);
  _lotserial->setEnabled(false);
  _description->setEnabled(false);
  _notes->setEnabled(false);
  _charass->setReadOnly(true);
  _owner->setEnabled(false);

  _buttonBox->setStandardButtons(QDialogButtonBox::Close);
  _comments->setReadOnly(true);
  _documents->setReadOnly(true);
  _alarms->setReadOnly(true);
}

int incident::id() const
{
  return _incdtid;
}

int incident::mode() const
{
  return _mode;
}

int incident::aropenid() const
{
  return _aropenid;
}

QString incident::arDoctype() const
{
  return _ardoctype;
}

void incident::sCancel()
{
  XSqlQuery incidentCancel;
  if (cNew == _mode)
  {
    incidentCancel.prepare("SELECT releaseNumber('IncidentNumber', :number) AS result;");
    incidentCancel.bindValue(":number", _number->text());
    incidentCancel.exec();
    if (incidentCancel.first())
    {
      int result = incidentCancel.value("result").toInt();
      if (result < 0)
      {
        ErrorReporter::error(QtCriticalMsg, this, tr("Error Cancelling Incident"),
                               storedProcErrorLookup("releaseNumber", result),
                               __FILE__, __LINE__);
        return;
      }
    }
    else if (ErrorReporter::error(QtCriticalMsg, this, tr("Error Cancelling Incident"),
                                  incidentCancel, __FILE__, __LINE__))
    {
      return;
    }

    incidentCancel.prepare("SELECT deleteIncident(:incdt_id) AS result;");
    incidentCancel.bindValue(":incdt_id", _incdtid);
    incidentCancel.exec();
    if (incidentCancel.first())
    {
      int result = incidentCancel.value("result").toInt();
      if (result < 0)
      {
        ErrorReporter::error(QtCriticalMsg, this, tr("Error Cancelling Incident"),
                               storedProcErrorLookup("deleteIncident", result),
                               __FILE__, __LINE__);
        return;
      }
    }
    else if (ErrorReporter::error(QtCriticalMsg, this, tr("Error Cancelling Incident"),
                                  incidentCancel, __FILE__, __LINE__))
    {
      return;
    }
  }

  reject();
}

void incident::sSave()
{
  if (! save(false)) // if error
    return;
  {
   done(_incdtid);
  }
}

bool incident::save(bool partial)
{
  XSqlQuery incidentave;
  if (! partial)
  {
    QList<GuiErrorCheck> errors;
    errors<< GuiErrorCheck(_crmacct->id() == -1, _number,
                           tr("You must specify the Account that this incident is for."))
          << GuiErrorCheck(_cntct->id() <= 0 && _cntct->name().simplified().isEmpty(), _cntct,
                           tr("You must specify a Contact for this Incident."))
          << GuiErrorCheck(_description->text().trimmed().isEmpty(), _description,
                           tr("You must specify a description for this incident report."))
          << GuiErrorCheck(_status->currentIndex() == 3 && _assignedTo->username().isEmpty(), _assignedTo,
                           tr("You must specify an assignee when the status is assigned."))
    ;
    if (GuiErrorCheck::reportErrors(this, tr("Cannot Save Incident"), errors))
      return false;
  }

  RecurrenceWidget::RecurrenceChangePolicy cp = _recurring->getChangePolicy();
  if (cp == RecurrenceWidget::NoPolicy)
    return false;

  XSqlQuery rollback;
  rollback.prepare("ROLLBACK;");

  if (!incidentave.exec("BEGIN"))
  {
    ErrorReporter::error(QtCriticalMsg, this, tr("Error Saving Incident"),
                         incidentave, __FILE__, __LINE__);
    return false;
  }

  if (cNew == _mode && !_saved)
    incidentave.prepare("INSERT INTO incdt"
              "      (incdt_id, incdt_number, incdt_crmacct_id, incdt_cntct_id,"
              "       incdt_summary, incdt_descrip, incdt_item_id,"
              "       incdt_status, incdt_assigned_username,"
              "       incdt_incdtcat_id, incdt_incdtseverity_id,"
              "       incdt_incdtpriority_id, incdt_incdtresolution_id,"
              "       incdt_ls_id, incdt_aropen_id, incdt_owner_username,"
              "       incdt_prj_id, incdt_public,"
              "       incdt_recurring_incdt_id) "
              "VALUES(:incdt_id, :incdt_number, :incdt_crmacct_id, :incdt_cntct_id,"
              "       COALESCE(:incdt_description, 'TEMP'||:incdt_number), "
              "       :incdt_notes, :incdt_item_id,"
              "       :incdt_status, :incdt_assigned_username,"
              "       :incdt_incdtcat_id, :incdt_incdtseverity_id,"
              "       :incdt_incdtpriority_id, :incdt_incdtresolution_id,"
              "       :incdt_ls_id, :incdt_aropen_id, :incdt_owner_username,"
              "       :incdt_prj_id, :incdt_public,"
              "       :incdt_recurring_incdt_id);" );
  else if (cEdit == _mode || _saved)
    incidentave.prepare("UPDATE incdt"
              "   SET incdt_cntct_id=:incdt_cntct_id,"
              "       incdt_crmacct_id=:incdt_crmacct_id,"
              "       incdt_summary=:incdt_description,"
              "       incdt_descrip=:incdt_notes,"
              "       incdt_item_id=:incdt_item_id,"
              "       incdt_status=:incdt_status,"
              "       incdt_assigned_username=:incdt_assigned_username,"
              "       incdt_incdtcat_id=:incdt_incdtcat_id,"
              "       incdt_incdtpriority_id=:incdt_incdtpriority_id,"
              "       incdt_incdtseverity_id=:incdt_incdtseverity_id,"
              "       incdt_incdtresolution_id=:incdt_incdtresolution_id,"
              "       incdt_ls_id=:incdt_ls_id,"
              "       incdt_owner_username=:incdt_owner_username,"
              "       incdt_prj_id=:incdt_prj_id,"
              "       incdt_public=:incdt_public,"
              "       incdt_recurring_incdt_id=:incdt_recurring_incdt_id"
              " WHERE (incdt_id=:incdt_id); ");

  incidentave.bindValue(":incdt_id", _incdtid);
  incidentave.bindValue(":incdt_number", _number->text());
  incidentave.bindValue(":incdt_owner_username", _owner->username());
  if (_crmacct->id() > 0)
    incidentave.bindValue(":incdt_crmacct_id", _crmacct->id());
  if (_cntct->id() > 0)
    incidentave.bindValue(":incdt_cntct_id", _cntct->id());
  if (!_description->text().trimmed().isEmpty())
    incidentave.bindValue(":incdt_description", _description->text().trimmed());
  incidentave.bindValue(":incdt_notes", _notes->toPlainText().trimmed());
  if(-1 != _item->id())
    incidentave.bindValue(":incdt_item_id", _item->id());
  incidentave.bindValue(":incdt_assigned_username", _assignedTo->username());
  incidentave.bindValue(":incdt_status", _statusCodes.at(_status->currentIndex()));
  if(_category->isValid())
    incidentave.bindValue(":incdt_incdtcat_id", _category->id());
  if(_severity->isValid())
    incidentave.bindValue(":incdt_incdtseverity_id", _severity->id());
  if(_priority->isValid())
    incidentave.bindValue(":incdt_incdtpriority_id", _priority->id());
  if(_resolution->isValid())
    incidentave.bindValue(":incdt_incdtresolution_id", _resolution->id());
  if ((_item->id() != -1) && (_lotserial->id() != -1))
    incidentave.bindValue(":incdt_ls_id", _lotserial->id());
  if (_aropenid > 0)
    incidentave.bindValue(":incdt_aropen_id", _aropenid);
  if (_recurring->isRecurring())
    incidentave.bindValue(":incdt_recurring_incdt_id", _recurring->parentId());
  if (_project->id() > 0)
    incidentave.bindValue(":incdt_prj_id", _project->id());
  incidentave.bindValue(":incdt_public", _public->isChecked());

  if(!incidentave.exec() && incidentave.lastError().type() != QSqlError::NoError)
  {
    rollback.exec();
    ErrorReporter::error(QtCriticalMsg, this, tr("Error Saving Incident"),
                         incidentave, __FILE__, __LINE__);
    return false;
  }

  QString errmsg;
  if (! _recurring->save(true, cp, &errmsg))
  {
    rollback.exec();
    ErrorReporter::error(QtCriticalMsg, this, tr("Error Saving Incident"),
                         rollback, __FILE__, __LINE__);
    return false;
  }

  incidentave.exec("COMMIT;");
  if(incidentave.lastError().type() != QSqlError::NoError)
  {
    rollback.exec();
    ErrorReporter::error(QtCriticalMsg, this, tr("Error Saving Incident"),
                         incidentave, __FILE__, __LINE__);
    return false;
  }

  _saved = true;
  return true;
}

void incident::sFillHistoryList()
{
  XSqlQuery incidentFillHistoryList;
  incidentFillHistoryList.prepare("SELECT * "
            "  FROM incdthist"
            " WHERE (incdthist_incdt_id=:incdt_id)"
            " ORDER BY incdthist_timestamp; ");
  incidentFillHistoryList.bindValue(":incdt_id", _incdtid);
  incidentFillHistoryList.bindValue(":new", tr("New Incident"));
  incidentFillHistoryList.bindValue(":status", tr("Status"));
  incidentFillHistoryList.bindValue(":category", tr("Category"));
  incidentFillHistoryList.bindValue(":severity", tr("Severity"));
  incidentFillHistoryList.bindValue(":priority", tr("Priority"));
  incidentFillHistoryList.bindValue(":resolution", tr("Resolution"));
  incidentFillHistoryList.bindValue(":assignedto", tr("Assigned To"));
  incidentFillHistoryList.bindValue(":notes", tr("Comment"));
  incidentFillHistoryList.bindValue(":contact", tr("Contact"));
  incidentFillHistoryList.exec();
  _incdthist->populate(incidentFillHistoryList);
  if (ErrorReporter::error(QtCriticalMsg, this, tr("Error Retrieving Incident History"),
                                incidentFillHistoryList, __FILE__, __LINE__))
  {
    return;
  }
}

void incident::populate()
{
  if (!_lock.acquire("incdt", _incdtid, AppLock::Interactive))
    setViewMode();

  _close = false;

  foreach (QWidget* widget, QApplication::allWidgets())
  {
    if (!widget->isWindow() || !widget->isVisible())
      continue;

    incident *w = qobject_cast<incident*>(widget);

    if (w && w->id()==_incdtid)
    {
      w->setFocus();

      if (omfgThis->showTopLevel())
      {
        w->raise();
        w->activateWindow();
      }

      _close = true;
      break;
    }
  }

  XSqlQuery incidentpopulate;
  incidentpopulate.prepare("SELECT incdt_number,"
            "       incdt_crmacct_id,"
            "       COALESCE(incdt_cntct_id,-1) AS incdt_cntct_id,"
            "       (cntct_first_name || ' ' || cntct_last_name) AS cntct_name,"
            "       incdt_summary, incdt_descrip,"
            "       incdt_item_id, incdt_ls_id,"
            "       incdt_status, incdt_assigned_username,"
            "       incdt_incdtcat_id, incdt_incdtseverity_id,"
            "       incdt_incdtpriority_id, incdt_incdtresolution_id,"
            "       incdt_owner_username, incdt_recurring_incdt_id,"
            "       COALESCE(incdt_aropen_id, -1) AS docId,"
            "       COALESCE(aropen_docnumber, '') AS docNumber,"
            "       CASE WHEN (aropen_doctype='C') THEN :creditMemo"
            "            WHEN (aropen_doctype='D') THEN :debitMemo"
            "            WHEN (aropen_doctype='I') THEN :invoice"
            "            WHEN (aropen_doctype='R') THEN :cashdeposit"
            "            ELSE ''"
            "       END AS docType, "
            "       COALESCE(incdt_prj_id,-1) AS incdt_prj_id,"
            "       COALESCE(incdt_public, false) AS incdt_public,"
            "       aropen_doctype "
            "FROM incdt LEFT OUTER JOIN cntct ON (incdt_cntct_id=cntct_id)"
            "           LEFT OUTER JOIN aropen ON (incdt_aropen_id=aropen_id) "
            "WHERE (incdt_id=:incdt_id); ");
  incidentpopulate.bindValue(":incdt_id", _incdtid);
  incidentpopulate.bindValue(":creditMemo", tr("Credit Memo"));
  incidentpopulate.bindValue(":debitMemo", tr("Debit Memo"));
  incidentpopulate.bindValue(":invoice", tr("Invoice"));
  incidentpopulate.bindValue(":cashdeposit", tr("Customer Deposit"));
  incidentpopulate.exec();
  if(incidentpopulate.first())
  {
    _cntct->setId(incidentpopulate.value("incdt_cntct_id").toInt());
    _crmacct->setId(incidentpopulate.value("incdt_crmacct_id").toInt());
    _owner->setUsername(incidentpopulate.value("incdt_owner_username").toString());
    _number->setText(incidentpopulate.value("incdt_number").toString());
    _assignedTo->setUsername(incidentpopulate.value("incdt_assigned_username").toString());
    _category->setNull();
    if(!incidentpopulate.value("incdt_incdtcat_id").toString().isEmpty())
      _category->setId(incidentpopulate.value("incdt_incdtcat_id").toInt());
    _status->setCurrentIndex(_statusCodes.indexOf(incidentpopulate.value("incdt_status").toString()));
    _severity->setNull();
    if(!incidentpopulate.value("incdt_incdtseverity_id").toString().isEmpty())
      _severity->setId(incidentpopulate.value("incdt_incdtseverity_id").toInt());
    _priority->setNull();
    if(!incidentpopulate.value("incdt_incdtpriority_id").toString().isEmpty())
      _priority->setId(incidentpopulate.value("incdt_incdtpriority_id").toInt());
    _resolution->setNull();
    if(!incidentpopulate.value("incdt_incdtresolution_id").toString().isEmpty())
      _resolution->setId(incidentpopulate.value("incdt_incdtresolution_id").toInt());
    if(!incidentpopulate.value("incdt_item_id").toString().isEmpty())
      _item->setId(incidentpopulate.value("incdt_item_id").toInt());
    else
      _item->setId(-1);
    if(!incidentpopulate.value("incdt_ls_id").toString().isEmpty())
      _lotserial->setId(incidentpopulate.value("incdt_ls_id").toInt());
    else
      _lotserial->setId(-1);
    _description->setText(incidentpopulate.value("incdt_summary").toString());
    _notes->setText(incidentpopulate.value("incdt_descrip").toString());

    _comments->setId(_incdtid);
    _documents->setId(_incdtid);
    _charass->setId(_incdtid);
    _alarms->setId(_incdtid);

    _project->setId(incidentpopulate.value("incdt_prj_id").toInt());
    _public->setChecked(incidentpopulate.value("incdt_public").toBool());
        
    _docType->setText(incidentpopulate.value("docType").toString());
    _docNumber->setText(incidentpopulate.value("docNumber").toString());
    _aropenid = incidentpopulate.value("docId").toInt();
    _ardoctype = incidentpopulate.value("aropen_doctype").toString();
    if (_aropenid > 0)
      _viewAR->setEnabled(true);

    _recurring->setParent(incidentpopulate.value("incdt_recurring_incdt_id").isNull() ?
                          _incdtid : incidentpopulate.value("incdt_recurring_incdt_id").toInt(),
                          "INCDT");

    sFillHistoryList();

    emit populated();
  }
}

void incident::sIncdtCategoryChanged(int newCat)
{
  if (!_saved)
  {
    if (! save(true))
      return;
  }

  XSqlQuery taskq;
  taskq.prepare("SELECT applyDefaultTasks('INCDT', :category, :incdt, :override) AS ret;" );
  taskq.bindValue(":category", newCat);
  taskq.bindValue(":incdt", _incdtid);
  taskq.bindValue(":override", false);
  taskq.exec();

  /*
   The following checks whether tasks already exist and should be overridden.
   return code 0 means no templates exist
   return code < 0 means tasks already exist and user is questions whether to override
   return code > 0 means no existing tasks so they have been copied automatically
  */
  if (taskq.first()) 
  {
    if (taskq.value("ret").toInt() < 0)
    {
      if (QMessageBox::question(this, tr("Existing Tasks"),
                         tr("<p>Tasks already exist for this Incident.\n"
                            "Do you want to replace tasks with the new template?"),
                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::No)
      {
          return;
      }

      taskq.bindValue(":override", true);
      taskq.exec();
      if (ErrorReporter::error(QtCriticalMsg, this, tr("Error Applying Template Tasks"),
                               taskq, __FILE__, __LINE__))
         return;
    }
  }
  else if (ErrorReporter::error(QtCriticalMsg, this, tr("Error Applying Template Tasks"),
                                taskq, __FILE__, __LINE__))
            return;

  _taskList->sFillList();

}

void incident::sCRMAcctChanged(const int newid)
{
  _cntct->setSearchAcct(newid);
}

void incident::sNewCharacteristic()
{
  if (! save(false))
    return;
  _charass->sNew();
}

void incident::sProjectUpdated()
{
  XSqlQuery updp;
  updp.prepare("UPDATE task SET task_prj_id=:prjid "
                 "WHERE task_parent_type='INCDT' "
                 "  AND task_parent_id=:incdtid;" );
  updp.bindValue(":prjid", _project->id());
  updp.bindValue(":incdtid", _incdtid);
  updp.exec();
  if (ErrorReporter::error(QtCriticalMsg, this, tr("Error Incident Project"),
                                updp, __FILE__, __LINE__))
       return;

  _taskList->parameterWidget()->setDefault(tr("Project"), _project->id(), true);
  _taskList->sFillList();
}

void incident::sIncidentHistory()
{
  ParameterList params;
  params.append("incdthist_id", _incdthist->id());

  incidentHistory newdlg(this, 0, true);
  newdlg.set(params);
  newdlg.exec();
}

void incident::sReturn()
{
  XSqlQuery incidentReturn;
  if (! save(true))
    return;

  ParameterList params;
  incidentReturn.prepare("SELECT rahead_id FROM rahead WHERE rahead_incdt_id=:incdt_id");
  incidentReturn.bindValue(":incdt_id", _incdtid);
  incidentReturn.exec();
  if(incidentReturn.first())
  {
    params.append("mode", "edit");
    params.append("rahead_id", incidentReturn.value("rahead_id").toInt());
  }
  else
  {
    params.append("mode", "new");
    params.append("incdt_id", _incdtid);
  }

  returnAuthorization * newdlg = new returnAuthorization(this);
  if(newdlg->set(params) == NoError)
    omfgThis->handleNewWindow(newdlg);
  else
    QMessageBox::critical(this, tr("Could Not Open Window"),
                          tr("The new Return Authorization could not be created"));
}

void incident::sViewAR()
{
  ParameterList params;
  params.append("mode", "view");
  params.append("aropen_id", _aropenid);

  arOpenItem newdlg(this, 0, true);
  newdlg.set(params);
  newdlg.exec();
}

void incident::sPrint()
{
  if (_incdtid != -1)
  {
    ParameterList params;
    params.append("incdt_id", _incdtid);
    params.append("print");

    orReport report("Incident", params);
    if (report.isValid())
      report.print();
    else
      report.reportError(this);
  }
//  ToDo
}

void incident::sAssigned()
{
  if (_status->currentIndex() < 3 && !_assignedTo->username().isEmpty())
    _status->setCurrentIndex(3);
}

void incident::setVisible(bool visible)
{
  if (_close)
    close();
  else
    XDialog::setVisible(visible);
}
