/*
 * This file is part of the xTuple ERP: PostBooks Edition, a free and
 * open source Enterprise Resource Planning software suite,
 * Copyright (c) 1999-2019 by OpenMFG LLC, d/b/a xTuple.
 * It is licensed to you under the Common Public Attribution License
 * version 1.0, the full text of which (including xTuple-specific Exhibits)
 * is available at www.xtuple.com/CPAL.  By using this software, you agree
 * to be bound by its terms.
 */

#ifndef DSPBACKLOG_H
#define DSPBACKLOG_H

#include "guiclient.h"
#include "display.h"

class dspBacklog : public display
{
    Q_OBJECT

public:
    dspBacklog(QWidget* parent = 0, const char* name = 0, Qt::WindowFlags fl = Qt::Window);

public slots:
    virtual SetResponse set(const ParameterList & pParams );
    virtual void sEditOrder();
    virtual void sViewOrder();
    virtual void sEditItem();
    virtual void sViewItem();
    virtual void sPrintPackingList();
    virtual void sAddToPackingListBatch();
    virtual void sPopulateMenu(QMenu * pMenu, QTreeWidgetItem*, int);
    virtual void sFillList();
};

#endif // DSPBACKLOG_H
