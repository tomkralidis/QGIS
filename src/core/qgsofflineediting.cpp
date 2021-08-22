/***************************************************************************
    offline_editing.cpp

    Offline Editing Plugin
    a QGIS plugin
     --------------------------------------
    Date                 : 22-Jul-2010
    Copyright            : (C) 2010 by Sourcepole
    Email                : info at sourcepole.ch
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/


#include "qgsapplication.h"
#include "qgsdatasourceuri.h"
#include "qgsgeometry.h"
#include "qgslayertreegroup.h"
#include "qgslayertreelayer.h"
#include "qgsmaplayer.h"
#include "qgsofflineediting.h"
#include "qgsproject.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayereditbuffer.h"
#include "qgsvectorlayerjoinbuffer.h"
#include "qgsspatialiteutils.h"
#include "qgsfeatureiterator.h"
#include "qgslogger.h"
#include "qgsvectorlayerutils.h"
#include "qgsrelationmanager.h"
#include "qgsmapthemecollection.h"
#include "qgslayertree.h"
#include "qgsogrutils.h"
#include "qgsvectorfilewriter.h"
#include "qgsvectorlayer.h"
#include "qgsproviderregistry.h"
#include "qgsprovidermetadata.h"
#include "qgsmaplayerstylemanager.h"
#include "qgsjsonutils.h"

#include <QDir>
#include <QDomDocument>
#include <QDomNode>
#include <QFile>
#include <QRegularExpression>

#include <ogr_srs_api.h>

extern "C"
{
#include <sqlite3.h>
}

#ifdef HAVE_SPATIALITE
extern "C"
{
#include <spatialite.h>
}
#endif

#define CUSTOM_PROPERTY_IS_OFFLINE_EDITABLE "isOfflineEditable"
#define CUSTOM_PROPERTY_REMOTE_SOURCE "remoteSource"
#define CUSTOM_PROPERTY_REMOTE_PROVIDER "remoteProvider"
#define CUSTOM_SHOW_FEATURE_COUNT "showFeatureCount"
#define CUSTOM_PROPERTY_ORIGINAL_LAYERID "remoteLayerId"
#define CUSTOM_PROPERTY_LAYERNAME_SUFFIX "layerNameSuffix"
#define PROJECT_ENTRY_SCOPE_OFFLINE "OfflineEditingPlugin"
#define PROJECT_ENTRY_KEY_OFFLINE_DB_PATH "/OfflineDbPath"

QgsOfflineEditing::QgsOfflineEditing()
{
  connect( QgsProject::instance(), &QgsProject::layerWasAdded, this, &QgsOfflineEditing::layerAdded );
}

/**
 * convert current project to offline project
 * returns offline project file path
 *
 * Workflow:
 *
 * - copy layers to SpatiaLite
 * - create SpatiaLite db at offlineDataPath
 * - create table for each layer
 * - add new SpatiaLite layer
 * - copy features
 * - save as offline project
 * - mark offline layers
 * - remove remote layers
 * - mark as offline project
 */
bool QgsOfflineEditing::convertToOfflineProject( const QString &offlineDataPath, const QString &offlineDbFile, const QStringList &layerIds, bool onlySelected, ContainerType containerType, const QString &layerNameSuffix )
{
  if ( layerIds.isEmpty() )
  {
    return false;
  }

  QString dbPath = QDir( offlineDataPath ).absoluteFilePath( offlineDbFile );
  if ( createOfflineDb( dbPath, containerType ) )
  {
    spatialite_database_unique_ptr database;
    int rc = database.open( dbPath );
    if ( rc != SQLITE_OK )
    {
      showWarning( tr( "Could not open the SpatiaLite database" ) );
    }
    else
    {
      // create logging tables
      createLoggingTables( database.get() );

      emit progressStarted();

      QMap<QString, QgsVectorJoinList > joinInfoBuffer;
      QMap<QString, QgsVectorLayer *> layerIdMapping;

      for ( const QString &layerId : layerIds )
      {
        QgsMapLayer *layer = QgsProject::instance()->mapLayer( layerId );
        QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( layer );
        if ( !vl || !vl->isValid() )
        {
          QgsDebugMsgLevel( QStringLiteral( "Layer %1 is invalid" ).arg( layerId ), 4 );
          continue;
        }
        QgsVectorJoinList joins = vl->vectorJoins();

        // Layer names will be appended an _offline suffix
        // Join fields are prefixed with the layer name and we do not want the
        // field name to change so we stabilize the field name by defining a
        // custom prefix with the layername without _offline suffix.
        QgsVectorJoinList::iterator joinIt = joins.begin();
        while ( joinIt != joins.end() )
        {
          if ( joinIt->prefix().isNull() )
          {
            QgsVectorLayer *vl = joinIt->joinLayer();

            if ( vl && vl->isValid() )
              joinIt->setPrefix( vl->name() + '_' );
          }
          ++joinIt;
        }
        joinInfoBuffer.insert( vl->id(), joins );
      }

      QgsSnappingConfig snappingConfig = QgsProject::instance()->snappingConfig();

      // copy selected vector layers to offline layer
      for ( int i = 0; i < layerIds.count(); i++ )
      {
        emit layerProgressUpdated( i + 1, layerIds.count() );

        QgsMapLayer *layer = QgsProject::instance()->mapLayer( layerIds.at( i ) );
        QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( layer );
        if ( vl && vl->isValid() )
        {
          QString origLayerId = vl->id();
          QgsVectorLayer *newLayer = copyVectorLayer( vl, database.get(), dbPath, onlySelected, containerType, layerNameSuffix );
          if ( newLayer && newLayer->isValid() )
          {
            layerIdMapping.insert( origLayerId, newLayer );
            //append individual layer setting on snapping settings
            snappingConfig.setIndividualLayerSettings( newLayer, snappingConfig.individualLayerSettings( vl ) );
            snappingConfig.removeLayers( QList<QgsMapLayer *>() << vl );

            // remove remote layer
            QgsProject::instance()->removeMapLayers(
              QStringList() << origLayerId );
          }
        }
      }

      QgsProject::instance()->setSnappingConfig( snappingConfig );

      // restore join info on new offline layer
      QMap<QString, QgsVectorJoinList >::ConstIterator it;
      for ( it = joinInfoBuffer.constBegin(); it != joinInfoBuffer.constEnd(); ++it )
      {
        QgsVectorLayer *newLayer = layerIdMapping.value( it.key() );

        if ( newLayer && newLayer->isValid() )
        {
          const QList<QgsVectorLayerJoinInfo> joins = it.value();
          for ( QgsVectorLayerJoinInfo join : joins )
          {
            QgsVectorLayer *newJoinedLayer = layerIdMapping.value( join.joinLayerId() );
            if ( newJoinedLayer && newJoinedLayer->isValid() )
            {
              // If the layer has been offline'd, update join information
              join.setJoinLayer( newJoinedLayer );
            }
            newLayer->addJoin( join );
          }
        }
      }

      emit progressStopped();

      // save offline project
      QString projectTitle = QgsProject::instance()->title();
      if ( projectTitle.isEmpty() )
      {
        projectTitle = QFileInfo( QgsProject::instance()->fileName() ).fileName();
      }
      projectTitle += QLatin1String( " (offline)" );
      QgsProject::instance()->setTitle( projectTitle );

      QgsProject::instance()->writeEntry( PROJECT_ENTRY_SCOPE_OFFLINE, PROJECT_ENTRY_KEY_OFFLINE_DB_PATH, QgsProject::instance()->writePath( dbPath ) );

      return true;
    }
  }

  return false;
}

bool QgsOfflineEditing::isOfflineProject() const
{
  return !QgsProject::instance()->readEntry( PROJECT_ENTRY_SCOPE_OFFLINE, PROJECT_ENTRY_KEY_OFFLINE_DB_PATH ).isEmpty();
}

void QgsOfflineEditing::synchronize()
{
  // open logging db
  sqlite3_database_unique_ptr database = openLoggingDb();
  if ( !database )
  {
    return;
  }

  emit progressStarted();

  QgsSnappingConfig snappingConfig = QgsProject::instance()->snappingConfig();

  // restore and sync remote layers
  QList<QgsMapLayer *> offlineLayers;
  QMap<QString, QgsMapLayer *> mapLayers = QgsProject::instance()->mapLayers();
  for ( QMap<QString, QgsMapLayer *>::iterator layer_it = mapLayers.begin() ; layer_it != mapLayers.end(); ++layer_it )
  {
    QgsMapLayer *layer = layer_it.value();

    if ( layer->customProperty( CUSTOM_PROPERTY_IS_OFFLINE_EDITABLE, false ).toBool() )
    {
      if ( !layer->isValid() )
      {
        QgsDebugMsgLevel( QStringLiteral( "Skipping offline layer %1 because it is an invalid layer" ).arg( layer->id() ), 4 );
        continue;
      }

      offlineLayers << layer;
    }
  }

  QgsDebugMsgLevel( QStringLiteral( "Found %1 offline layers" ).arg( offlineLayers.count() ), 4 );
  for ( int l = 0; l < offlineLayers.count(); l++ )
  {
    QgsMapLayer *layer = offlineLayers.at( l );

    emit layerProgressUpdated( l + 1, offlineLayers.count() );

    QString remoteSource = layer->customProperty( CUSTOM_PROPERTY_REMOTE_SOURCE, "" ).toString();
    QString remoteProvider = layer->customProperty( CUSTOM_PROPERTY_REMOTE_PROVIDER, "" ).toString();
    QString remoteName = layer->name();
    QString remoteNameSuffix = layer->customProperty( CUSTOM_PROPERTY_LAYERNAME_SUFFIX, " (offline)" ).toString();
    if ( remoteName.endsWith( remoteNameSuffix ) )
      remoteName.chop( remoteNameSuffix.size() );
    const QgsVectorLayer::LayerOptions options { QgsProject::instance()->transformContext() };
    QgsVectorLayer *remoteLayer = new QgsVectorLayer( remoteSource, remoteName, remoteProvider, options );
    if ( remoteLayer->isValid() )
    {
      // Rebuild WFS cache to get feature id<->GML fid mapping
      if ( remoteLayer->providerType().contains( QLatin1String( "WFS" ), Qt::CaseInsensitive ) )
      {
        QgsFeatureIterator fit = remoteLayer->getFeatures();
        QgsFeature f;
        while ( fit.nextFeature( f ) )
        {
        }
      }
      // TODO: only add remote layer if there are log entries?

      QgsVectorLayer *offlineLayer = qobject_cast<QgsVectorLayer *>( layer );

      if ( offlineLayer->isValid() )
      {
        // register this layer with the central layers registry
        QgsProject::instance()->addMapLayers( QList<QgsMapLayer *>() << remoteLayer, true );

        // copy style
        copySymbology( offlineLayer, remoteLayer );
        updateRelations( offlineLayer, remoteLayer );
        updateMapThemes( offlineLayer, remoteLayer );
        updateLayerOrder( offlineLayer, remoteLayer );

        //append individual layer setting on snapping settings
        snappingConfig.setIndividualLayerSettings( remoteLayer, snappingConfig.individualLayerSettings( offlineLayer ) );
        snappingConfig.removeLayers( QList<QgsMapLayer *>() << offlineLayer );

        //set QgsLayerTreeNode properties back
        QgsLayerTreeLayer *layerTreeLayer = QgsProject::instance()->layerTreeRoot()->findLayer( offlineLayer->id() );
        QgsLayerTreeLayer *newLayerTreeLayer = QgsProject::instance()->layerTreeRoot()->findLayer( remoteLayer->id() );
        newLayerTreeLayer->setCustomProperty( CUSTOM_SHOW_FEATURE_COUNT, layerTreeLayer->customProperty( CUSTOM_SHOW_FEATURE_COUNT ) );

        // apply layer edit log
        QString qgisLayerId = layer->id();
        QString sql = QStringLiteral( "SELECT \"id\" FROM 'log_layer_ids' WHERE \"qgis_id\" = '%1'" ).arg( qgisLayerId );
        int layerId = sqlQueryInt( database.get(), sql, -1 );
        if ( layerId != -1 )
        {
          remoteLayer->startEditing();

          // TODO: only get commitNos of this layer?
          int commitNo = getCommitNo( database.get() );
          QgsDebugMsgLevel( QStringLiteral( "Found %1 commits" ).arg( commitNo ), 4 );
          for ( int i = 0; i < commitNo; i++ )
          {
            QgsDebugMsgLevel( QStringLiteral( "Apply commits chronologically" ), 4 );
            // apply commits chronologically
            applyAttributesAdded( remoteLayer, database.get(), layerId, i );
            applyAttributeValueChanges( offlineLayer, remoteLayer, database.get(), layerId, i );
            applyGeometryChanges( remoteLayer, database.get(), layerId, i );
          }

          applyFeaturesAdded( offlineLayer, remoteLayer, database.get(), layerId );
          applyFeaturesRemoved( remoteLayer, database.get(), layerId );

          if ( remoteLayer->commitChanges() )
          {
            // update fid lookup
            updateFidLookup( remoteLayer, database.get(), layerId );

            // clear edit log for this layer
            sql = QStringLiteral( "DELETE FROM 'log_added_attrs' WHERE \"layer_id\" = %1" ).arg( layerId );
            sqlExec( database.get(), sql );
            sql = QStringLiteral( "DELETE FROM 'log_added_features' WHERE \"layer_id\" = %1" ).arg( layerId );
            sqlExec( database.get(), sql );
            sql = QStringLiteral( "DELETE FROM 'log_removed_features' WHERE \"layer_id\" = %1" ).arg( layerId );
            sqlExec( database.get(), sql );
            sql = QStringLiteral( "DELETE FROM 'log_feature_updates' WHERE \"layer_id\" = %1" ).arg( layerId );
            sqlExec( database.get(), sql );
            sql = QStringLiteral( "DELETE FROM 'log_geometry_updates' WHERE \"layer_id\" = %1" ).arg( layerId );
            sqlExec( database.get(), sql );
          }
          else
          {
            showWarning( remoteLayer->commitErrors().join( QLatin1Char( '\n' ) ) );
          }
        }
        else
        {
          QgsDebugMsg( QStringLiteral( "Could not find the layer id in the edit logs!" ) );
        }
        // Invalidate the connection to force a reload if the project is put offline
        // again with the same path
        offlineLayer->dataProvider()->invalidateConnections( QgsDataSourceUri( offlineLayer->source() ).database() );
        // remove offline layer
        QgsProject::instance()->removeMapLayers( QStringList() << qgisLayerId );


        // disable offline project
        QString projectTitle = QgsProject::instance()->title();
        projectTitle.remove( QRegularExpression( " \\(offline\\)$" ) );
        QgsProject::instance()->setTitle( projectTitle );
        QgsProject::instance()->removeEntry( PROJECT_ENTRY_SCOPE_OFFLINE, PROJECT_ENTRY_KEY_OFFLINE_DB_PATH );
        remoteLayer->reload(); //update with other changes
      }
      else
      {
        QgsDebugMsg( QStringLiteral( "Offline layer %1 is not valid!" ).arg( offlineLayer->id() ) );
      }
    }
    else
    {
      QgsDebugMsg( QStringLiteral( "Remote layer %1 is not valid!" ).arg( remoteLayer->id() ) );
    }
  }

  // reset commitNo
  QString sql = QStringLiteral( "UPDATE 'log_indices' SET 'last_index' = 0 WHERE \"name\" = 'commit_no'" );
  sqlExec( database.get(), sql );

  QgsProject::instance()->setSnappingConfig( snappingConfig );

  emit progressStopped();
}

void QgsOfflineEditing::initializeSpatialMetadata( sqlite3 *sqlite_handle )
{
#ifdef HAVE_SPATIALITE
  // attempting to perform self-initialization for a newly created DB
  if ( !sqlite_handle )
    return;
  // checking if this DB is really empty
  char **results = nullptr;
  int rows, columns;
  int ret = sqlite3_get_table( sqlite_handle, "select count(*) from sqlite_master", &results, &rows, &columns, nullptr );
  if ( ret != SQLITE_OK )
    return;
  int count = 0;
  if ( rows >= 1 )
  {
    for ( int i = 1; i <= rows; i++ )
      count = atoi( results[( i * columns ) + 0] );
  }

  sqlite3_free_table( results );

  if ( count > 0 )
    return;

  bool above41 = false;
  ret = sqlite3_get_table( sqlite_handle, "select spatialite_version()", &results, &rows, &columns, nullptr );
  if ( ret == SQLITE_OK && rows == 1 && columns == 1 )
  {
    QString version = QString::fromUtf8( results[1] );
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    QStringList parts = version.split( ' ', QString::SkipEmptyParts );
#else
    QStringList parts = version.split( ' ', Qt::SkipEmptyParts );
#endif
    if ( !parts.empty() )
    {
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
      QStringList verparts = parts.at( 0 ).split( '.', QString::SkipEmptyParts );
#else
      QStringList verparts = parts.at( 0 ).split( '.', Qt::SkipEmptyParts );
#endif
      above41 = verparts.size() >= 2 && ( verparts.at( 0 ).toInt() > 4 || ( verparts.at( 0 ).toInt() == 4 && verparts.at( 1 ).toInt() >= 1 ) );
    }
  }

  sqlite3_free_table( results );

  // all right, it's empty: proceeding to initialize
  char *errMsg = nullptr;
  ret = sqlite3_exec( sqlite_handle, above41 ? "SELECT InitSpatialMetadata(1)" : "SELECT InitSpatialMetadata()", nullptr, nullptr, &errMsg );

  if ( ret != SQLITE_OK )
  {
    QString errCause = tr( "Unable to initialize SpatialMetadata:\n" );
    errCause += QString::fromUtf8( errMsg );
    showWarning( errCause );
    sqlite3_free( errMsg );
    return;
  }
  spatial_ref_sys_init( sqlite_handle, 0 );
#else
  ( void )sqlite_handle;
#endif
}

bool QgsOfflineEditing::createOfflineDb( const QString &offlineDbPath, ContainerType containerType )
{
  int ret;
  char *errMsg = nullptr;
  QFile newDb( offlineDbPath );
  if ( newDb.exists() )
  {
    QFile::remove( offlineDbPath );
  }

  // see also QgsNewSpatialiteLayerDialog::createDb()

  QFileInfo fullPath = QFileInfo( offlineDbPath );
  QDir path = fullPath.dir();

  // Must be sure there is destination directory ~/.qgis
  QDir().mkpath( path.absolutePath() );

  // creating/opening the new database
  QString dbPath = newDb.fileName();

  // creating geopackage
  switch ( containerType )
  {
    case GPKG:
    {
      OGRSFDriverH hGpkgDriver = OGRGetDriverByName( "GPKG" );
      if ( !hGpkgDriver )
      {
        showWarning( tr( "Creation of database failed. GeoPackage driver not found." ) );
        return false;
      }

      gdal::ogr_datasource_unique_ptr hDS( OGR_Dr_CreateDataSource( hGpkgDriver, dbPath.toUtf8().constData(), nullptr ) );
      if ( !hDS )
      {
        showWarning( tr( "Creation of database failed (OGR error: %1)" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
        return false;
      }
      break;
    }
    case SpatiaLite:
    {
      break;
    }
  }

  spatialite_database_unique_ptr database;
  ret = database.open_v2( dbPath, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr );
  if ( ret )
  {
    // an error occurred
    QString errCause = tr( "Could not create a new database\n" );
    errCause += database.errorMessage();
    showWarning( errCause );
    return false;
  }
  // activating Foreign Key constraints
  ret = sqlite3_exec( database.get(), "PRAGMA foreign_keys = 1", nullptr, nullptr, &errMsg );
  if ( ret != SQLITE_OK )
  {
    showWarning( tr( "Unable to activate FOREIGN_KEY constraints" ) );
    sqlite3_free( errMsg );
    return false;
  }
  initializeSpatialMetadata( database.get() );
  return true;
}

void QgsOfflineEditing::createLoggingTables( sqlite3 *db )
{
  // indices
  QString sql = QStringLiteral( "CREATE TABLE 'log_indices' ('name' TEXT, 'last_index' INTEGER)" );
  sqlExec( db, sql );

  sql = QStringLiteral( "INSERT INTO 'log_indices' VALUES ('commit_no', 0)" );
  sqlExec( db, sql );

  sql = QStringLiteral( "INSERT INTO 'log_indices' VALUES ('layer_id', 0)" );
  sqlExec( db, sql );

  // layername <-> layer id
  sql = QStringLiteral( "CREATE TABLE 'log_layer_ids' ('id' INTEGER, 'qgis_id' TEXT)" );
  sqlExec( db, sql );

  // offline fid <-> remote fid
  sql = QStringLiteral( "CREATE TABLE 'log_fids' ('layer_id' INTEGER, 'offline_fid' INTEGER, 'remote_fid' INTEGER)" );
  sqlExec( db, sql );

  // added attributes
  sql = QStringLiteral( "CREATE TABLE 'log_added_attrs' ('layer_id' INTEGER, 'commit_no' INTEGER, " );
  sql += QLatin1String( "'name' TEXT, 'type' INTEGER, 'length' INTEGER, 'precision' INTEGER, 'comment' TEXT)" );
  sqlExec( db, sql );

  // added features
  sql = QStringLiteral( "CREATE TABLE 'log_added_features' ('layer_id' INTEGER, 'fid' INTEGER)" );
  sqlExec( db, sql );

  // removed features
  sql = QStringLiteral( "CREATE TABLE 'log_removed_features' ('layer_id' INTEGER, 'fid' INTEGER)" );
  sqlExec( db, sql );

  // feature updates
  sql = QStringLiteral( "CREATE TABLE 'log_feature_updates' ('layer_id' INTEGER, 'commit_no' INTEGER, 'fid' INTEGER, 'attr' INTEGER, 'value' TEXT)" );
  sqlExec( db, sql );

  // geometry updates
  sql = QStringLiteral( "CREATE TABLE 'log_geometry_updates' ('layer_id' INTEGER, 'commit_no' INTEGER, 'fid' INTEGER, 'geom_wkt' TEXT)" );
  sqlExec( db, sql );

  /* TODO: other logging tables
    - attr delete (not supported by SpatiaLite provider)
  */
}

QgsVectorLayer *QgsOfflineEditing::copyVectorLayer( QgsVectorLayer *layer, sqlite3 *db, const QString &offlineDbPath, bool onlySelected, ContainerType containerType, const QString &layerNameSuffix )
{
  if ( !layer || !layer->isValid() )
  {
    QgsDebugMsgLevel( QStringLiteral( "Layer %1 is invalid and cannot be copied" ).arg( layer ? layer->id() : QStringLiteral( "<UNKNOWN>" ) ), 4 );
    return nullptr;
  }

  QString tableName = layer->id();
  QgsDebugMsgLevel( QStringLiteral( "Creating offline table %1 ..." ).arg( tableName ), 4 );

  // new layer
  QgsVectorLayer *newLayer = nullptr;

  switch ( containerType )
  {
    case SpatiaLite:
    {
#ifdef HAVE_SPATIALITE
      // create table
      QString sql = QStringLiteral( "CREATE TABLE '%1' (" ).arg( tableName );
      QString delim;
      const QgsFields providerFields = layer->dataProvider()->fields();
      for ( const auto &field : providerFields )
      {
        QString dataType;
        QVariant::Type type = field.type();
        if ( type == QVariant::Int || type == QVariant::LongLong )
        {
          dataType = QStringLiteral( "INTEGER" );
        }
        else if ( type == QVariant::Double )
        {
          dataType = QStringLiteral( "REAL" );
        }
        else if ( type == QVariant::String )
        {
          dataType = QStringLiteral( "TEXT" );
        }
        else if ( type == QVariant::StringList  || type == QVariant::List )
        {
          dataType = QStringLiteral( "TEXT" );
          showWarning( tr( "Field '%1' from layer %2 has been converted from a list to a string of comma-separated values." ).arg( field.name(), layer->name() ) );
        }
        else
        {
          showWarning( tr( "%1: Unknown data type %2. Not using type affinity for the field." ).arg( field.name(), QVariant::typeToName( type ) ) );
        }

        sql += delim + QStringLiteral( "'%1' %2" ).arg( field.name(), dataType );
        delim = ',';
      }
      sql += ')';

      int rc = sqlExec( db, sql );

      // add geometry column
      if ( layer->isSpatial() )
      {
        const QgsWkbTypes::Type sourceWkbType = layer->wkbType();

        QString geomType;
        switch ( QgsWkbTypes::flatType( sourceWkbType ) )
        {
          case QgsWkbTypes::Point:
            geomType = QStringLiteral( "POINT" );
            break;
          case QgsWkbTypes::MultiPoint:
            geomType = QStringLiteral( "MULTIPOINT" );
            break;
          case QgsWkbTypes::LineString:
            geomType = QStringLiteral( "LINESTRING" );
            break;
          case QgsWkbTypes::MultiLineString:
            geomType = QStringLiteral( "MULTILINESTRING" );
            break;
          case QgsWkbTypes::Polygon:
            geomType = QStringLiteral( "POLYGON" );
            break;
          case QgsWkbTypes::MultiPolygon:
            geomType = QStringLiteral( "MULTIPOLYGON" );
            break;
          default:
            showWarning( tr( "Layer %1 has unsupported geometry type %2." ).arg( layer->name(), QgsWkbTypes::displayString( layer->wkbType() ) ) );
            break;
        };

        QString zmInfo = QStringLiteral( "XY" );

        if ( QgsWkbTypes::hasZ( sourceWkbType ) )
          zmInfo += 'Z';
        if ( QgsWkbTypes::hasM( sourceWkbType ) )
          zmInfo += 'M';

        QString epsgCode;

        if ( layer->crs().authid().startsWith( QLatin1String( "EPSG:" ), Qt::CaseInsensitive ) )
        {
          epsgCode = layer->crs().authid().mid( 5 );
        }
        else
        {
          epsgCode = '0';
          showWarning( tr( "Layer %1 has unsupported Coordinate Reference System (%2)." ).arg( layer->name(), layer->crs().authid() ) );
        }

        QString sqlAddGeom = QStringLiteral( "SELECT AddGeometryColumn('%1', 'Geometry', %2, '%3', '%4')" )
                             .arg( tableName, epsgCode, geomType, zmInfo );

        // create spatial index
        QString sqlCreateIndex = QStringLiteral( "SELECT CreateSpatialIndex('%1', 'Geometry')" ).arg( tableName );

        if ( rc == SQLITE_OK )
        {
          rc = sqlExec( db, sqlAddGeom );
          if ( rc == SQLITE_OK )
          {
            rc = sqlExec( db, sqlCreateIndex );
          }
        }
      }

      if ( rc != SQLITE_OK )
      {
        showWarning( tr( "Filling SpatiaLite for layer %1 failed" ).arg( layer->name() ) );
        return nullptr;
      }

      // add new layer
      QString connectionString = QStringLiteral( "dbname='%1' table='%2'%3 sql=" )
                                 .arg( offlineDbPath,
                                       tableName, layer->isSpatial() ? "(Geometry)" : "" );
      QgsVectorLayer::LayerOptions options { QgsProject::instance()->transformContext() };
      newLayer = new QgsVectorLayer( connectionString,
                                     layer->name() + layerNameSuffix, QStringLiteral( "spatialite" ), options );
      break;

#else
      showWarning( tr( "No Spatialite support available" ) );
      return nullptr;
#endif
    }

    case GPKG:
    {
      // Set options
      char **options = nullptr;

      options = CSLSetNameValue( options, "OVERWRITE", "YES" );
      options = CSLSetNameValue( options, "IDENTIFIER", tr( "%1 (offline)" ).arg( layer->id() ).toUtf8().constData() );
      options = CSLSetNameValue( options, "DESCRIPTION", layer->dataComment().toUtf8().constData() );

      //the FID-name should not exist in the original data
      QString fidBase( QStringLiteral( "fid" ) );
      QString fid = fidBase;
      int counter = 1;
      while ( layer->dataProvider()->fields().lookupField( fid ) >= 0 && counter < 10000 )
      {
        fid = fidBase + '_' + QString::number( counter );
        counter++;
      }
      if ( counter == 10000 )
      {
        showWarning( tr( "Cannot make FID-name for GPKG " ) );
        return nullptr;
      }

      options = CSLSetNameValue( options, "FID", fid.toUtf8().constData() );

      if ( layer->isSpatial() )
      {
        options = CSLSetNameValue( options, "GEOMETRY_COLUMN", "geom" );
        options = CSLSetNameValue( options, "SPATIAL_INDEX", "YES" );
      }

      OGRSFDriverH hDriver = nullptr;
      OGRSpatialReferenceH hSRS = OSRNewSpatialReference( layer->crs().toWkt( QgsCoordinateReferenceSystem::WKT_PREFERRED_GDAL ).toLocal8Bit().data() );
      gdal::ogr_datasource_unique_ptr hDS( OGROpen( offlineDbPath.toUtf8().constData(), true, &hDriver ) );
      OGRLayerH hLayer = OGR_DS_CreateLayer( hDS.get(), tableName.toUtf8().constData(), hSRS, static_cast<OGRwkbGeometryType>( layer->wkbType() ), options );
      CSLDestroy( options );
      if ( hSRS )
        OSRRelease( hSRS );
      if ( !hLayer )
      {
        showWarning( tr( "Creation of layer failed (OGR error: %1)" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
        return nullptr;
      }

      const QgsFields providerFields = layer->dataProvider()->fields();
      for ( const auto &field : providerFields )
      {
        const QString fieldName( field.name() );
        const QVariant::Type type = field.type();
        OGRFieldType ogrType( OFTString );
        OGRFieldSubType ogrSubType = OFSTNone;
        if ( type == QVariant::Int )
          ogrType = OFTInteger;
        else if ( type == QVariant::LongLong )
          ogrType = OFTInteger64;
        else if ( type == QVariant::Double )
          ogrType = OFTReal;
        else if ( type == QVariant::Time )
          ogrType = OFTTime;
        else if ( type == QVariant::Date )
          ogrType = OFTDate;
        else if ( type == QVariant::DateTime )
          ogrType = OFTDateTime;
        else if ( type == QVariant::Bool )
        {
          ogrType = OFTInteger;
          ogrSubType = OFSTBoolean;
        }
        else if ( type == QVariant::StringList || type == QVariant::List )
        {
          ogrType = OFTString;
          ogrSubType = OFSTJSON;
          showWarning( tr( "Field '%1' from layer %2 has been converted from a list to a JSON-formatted string value." ).arg( fieldName, layer->name() ) );
        }
        else
          ogrType = OFTString;

        int ogrWidth = field.length();

        gdal::ogr_field_def_unique_ptr fld( OGR_Fld_Create( fieldName.toUtf8().constData(), ogrType ) );
        OGR_Fld_SetWidth( fld.get(), ogrWidth );
        if ( ogrSubType != OFSTNone )
          OGR_Fld_SetSubType( fld.get(), ogrSubType );

        if ( OGR_L_CreateField( hLayer, fld.get(), true ) != OGRERR_NONE )
        {
          showWarning( tr( "Creation of field %1 failed (OGR error: %2)" )
                       .arg( fieldName, QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
          return nullptr;
        }
      }

      // In GDAL >= 2.0, the driver implements a deferred creation strategy, so
      // issue a command that will force table creation
      CPLErrorReset();
      OGR_L_ResetReading( hLayer );
      if ( CPLGetLastErrorType() != CE_None )
      {
        QString msg( tr( "Creation of layer failed (OGR error: %1)" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
        showWarning( msg );
        return nullptr;
      }
      hDS.reset();

      QString uri = QStringLiteral( "%1|layername=%2" ).arg( offlineDbPath,  tableName );
      QgsVectorLayer::LayerOptions layerOptions { QgsProject::instance()->transformContext() };
      newLayer = new QgsVectorLayer( uri, layer->name() + layerNameSuffix, QStringLiteral( "ogr" ), layerOptions );
      break;
    }
  }

  if ( newLayer && newLayer->isValid() )
  {

    // copy features
    newLayer->startEditing();
    QgsFeature f;

    QgsFeatureRequest req;

    if ( onlySelected )
    {
      QgsFeatureIds selectedFids = layer->selectedFeatureIds();
      if ( !selectedFids.isEmpty() )
        req.setFilterFids( selectedFids );
    }

    QgsFeatureIterator fit = layer->dataProvider()->getFeatures( req );

    if ( req.filterType() == QgsFeatureRequest::FilterFids )
    {
      emit progressModeSet( QgsOfflineEditing::CopyFeatures, layer->selectedFeatureIds().size() );
    }
    else
    {
      emit progressModeSet( QgsOfflineEditing::CopyFeatures, layer->dataProvider()->featureCount() );
    }
    long long featureCount = 1;

    QList<QgsFeatureId> remoteFeatureIds;
    while ( fit.nextFeature( f ) )
    {
      remoteFeatureIds << f.id();

      // NOTE: SpatiaLite provider ignores position of geometry column
      // fill gap in QgsAttributeMap if geometry column is not last (WORKAROUND)
      int column = 0;
      QgsAttributes attrs = f.attributes();
      // on GPKG newAttrs has an addition FID attribute, so we have to add a dummy in the original set
      QgsAttributes newAttrs( containerType == GPKG ? attrs.count() + 1 : attrs.count() );
      for ( int it = 0; it < attrs.count(); ++it )
      {
        QVariant attr = attrs.at( it );
        if ( layer->fields().at( it ).type() == QVariant::StringList || layer->fields().at( it ).type() == QVariant::List )
        {
          attr = QgsJsonUtils::encodeValue( attr );
        }
        newAttrs[column++] = attr;
      }
      f.setAttributes( newAttrs );

      newLayer->addFeature( f );

      emit progressUpdated( featureCount++ );
    }
    if ( newLayer->commitChanges() )
    {
      emit progressModeSet( QgsOfflineEditing::ProcessFeatures, layer->dataProvider()->featureCount() );
      featureCount = 1;

      // update feature id lookup
      int layerId = getOrCreateLayerId( db, newLayer->id() );
      QList<QgsFeatureId> offlineFeatureIds;

      QgsFeatureIterator fit = newLayer->getFeatures( QgsFeatureRequest().setFlags( QgsFeatureRequest::NoGeometry ).setNoAttributes() );
      while ( fit.nextFeature( f ) )
      {
        offlineFeatureIds << f.id();
      }

      // NOTE: insert fids in this loop, as the db is locked during newLayer->nextFeature()
      sqlExec( db, QStringLiteral( "BEGIN" ) );
      int remoteCount = remoteFeatureIds.size();
      for ( int i = 0; i < remoteCount; i++ )
      {
        // Check if the online feature has been fetched (WFS download aborted for some reason)
        if ( i < offlineFeatureIds.count() )
        {
          addFidLookup( db, layerId, offlineFeatureIds.at( i ), remoteFeatureIds.at( i ) );
        }
        else
        {
          showWarning( tr( "Feature cannot be copied to the offline layer, please check if the online layer '%1' is still accessible." ).arg( layer->name() ) );
          return nullptr;
        }
        emit progressUpdated( featureCount++ );
      }
      sqlExec( db, QStringLiteral( "COMMIT" ) );
    }
    else
    {
      showWarning( newLayer->commitErrors().join( QLatin1Char( '\n' ) ) );
    }

    // copy the custom properties from original layer
    newLayer->setCustomProperties( layer->customProperties() );

    // mark as offline layer
    newLayer->setCustomProperty( CUSTOM_PROPERTY_IS_OFFLINE_EDITABLE, true );

    // store original layer source and information
    newLayer->setCustomProperty( CUSTOM_PROPERTY_REMOTE_SOURCE, layer->source() );
    newLayer->setCustomProperty( CUSTOM_PROPERTY_REMOTE_PROVIDER, layer->providerType() );
    newLayer->setCustomProperty( CUSTOM_PROPERTY_ORIGINAL_LAYERID, layer->id() );
    newLayer->setCustomProperty( CUSTOM_PROPERTY_LAYERNAME_SUFFIX, layerNameSuffix );

    // register this layer with the central layers registry
    QgsProject::instance()->addMapLayers(
      QList<QgsMapLayer *>() << newLayer );

    // copy style
    copySymbology( layer, newLayer );

    //remove constrainst of fields that use defaultValueClauses from provider on original
    const auto fields = layer->fields();
    for ( const QgsField &field : fields )
    {
      if ( !layer->dataProvider()->defaultValueClause( layer->fields().fieldOriginIndex( layer->fields().indexOf( field.name() ) ) ).isEmpty() )
      {
        newLayer->removeFieldConstraint( newLayer->fields().indexOf( field.name() ), QgsFieldConstraints::ConstraintNotNull );
      }
    }

    QgsLayerTreeGroup *layerTreeRoot = QgsProject::instance()->layerTreeRoot();
    // Find the parent group of the original layer
    QgsLayerTreeLayer *layerTreeLayer = layerTreeRoot->findLayer( layer->id() );
    if ( layerTreeLayer )
    {
      QgsLayerTreeGroup *parentTreeGroup = qobject_cast<QgsLayerTreeGroup *>( layerTreeLayer->parent() );
      if ( parentTreeGroup )
      {
        int index = parentTreeGroup->children().indexOf( layerTreeLayer );
        // Move the new layer from the root group to the new group
        QgsLayerTreeLayer *newLayerTreeLayer = layerTreeRoot->findLayer( newLayer->id() );
        if ( newLayerTreeLayer )
        {
          QgsLayerTreeNode *newLayerTreeLayerClone = newLayerTreeLayer->clone();
          //copy the showFeatureCount property to the new node
          newLayerTreeLayerClone->setCustomProperty( CUSTOM_SHOW_FEATURE_COUNT, layerTreeLayer->customProperty( CUSTOM_SHOW_FEATURE_COUNT ) );
          newLayerTreeLayerClone->setItemVisibilityChecked( layerTreeLayer->isVisible() );
          QgsLayerTreeGroup *grp = qobject_cast<QgsLayerTreeGroup *>( newLayerTreeLayer->parent() );
          parentTreeGroup->insertChildNode( index, newLayerTreeLayerClone );
          if ( grp )
            grp->removeChildNode( newLayerTreeLayer );
        }
      }
    }

    updateRelations( layer, newLayer );
    updateMapThemes( layer, newLayer );
    updateLayerOrder( layer, newLayer );



  }
  return newLayer;
}

void QgsOfflineEditing::applyAttributesAdded( QgsVectorLayer *remoteLayer, sqlite3 *db, int layerId, int commitNo )
{
  Q_ASSERT( remoteLayer );

  QString sql = QStringLiteral( "SELECT \"name\", \"type\", \"length\", \"precision\", \"comment\" FROM 'log_added_attrs' WHERE \"layer_id\" = %1 AND \"commit_no\" = %2" ).arg( layerId ).arg( commitNo );
  QList<QgsField> fields = sqlQueryAttributesAdded( db, sql );

  const QgsVectorDataProvider *provider = remoteLayer->dataProvider();
  QList<QgsVectorDataProvider::NativeType> nativeTypes = provider->nativeTypes();

  // NOTE: uses last matching QVariant::Type of nativeTypes
  QMap < QVariant::Type, QString /*typeName*/ > typeNameLookup;
  for ( int i = 0; i < nativeTypes.size(); i++ )
  {
    QgsVectorDataProvider::NativeType nativeType = nativeTypes.at( i );
    typeNameLookup[ nativeType.mType ] = nativeType.mTypeName;
  }

  emit progressModeSet( QgsOfflineEditing::AddFields, fields.size() );

  for ( int i = 0; i < fields.size(); i++ )
  {
    // lookup typename from layer provider
    QgsField field = fields[i];
    if ( typeNameLookup.contains( field.type() ) )
    {
      QString typeName = typeNameLookup[ field.type()];
      field.setTypeName( typeName );
      remoteLayer->addAttribute( field );
    }
    else
    {
      showWarning( QStringLiteral( "Could not add attribute '%1' of type %2" ).arg( field.name() ).arg( field.type() ) );
    }

    emit progressUpdated( i + 1 );
  }
}

void QgsOfflineEditing::applyFeaturesAdded( QgsVectorLayer *offlineLayer, QgsVectorLayer *remoteLayer, sqlite3 *db, int layerId )
{
  Q_ASSERT( offlineLayer );
  Q_ASSERT( remoteLayer );

  QString sql = QStringLiteral( "SELECT \"fid\" FROM 'log_added_features' WHERE \"layer_id\" = %1" ).arg( layerId );
  const QList<int> featureIdInts = sqlQueryInts( db, sql );
  QgsFeatureIds newFeatureIds;
  for ( int id : featureIdInts )
  {
    newFeatureIds << id;
  }

  QgsExpressionContext context = remoteLayer->createExpressionContext();

  // get new features from offline layer
  QgsFeatureList features;
  QgsFeatureIterator it = offlineLayer->getFeatures( QgsFeatureRequest().setFilterFids( newFeatureIds ) );
  QgsFeature feature;
  while ( it.nextFeature( feature ) )
  {
    features << feature;
  }

  // copy features to remote layer
  emit progressModeSet( QgsOfflineEditing::AddFeatures, features.size() );

  int i = 1;
  int newAttrsCount = remoteLayer->fields().count();
  for ( QgsFeatureList::iterator it = features.begin(); it != features.end(); ++it )
  {
    // NOTE: SpatiaLite provider ignores position of geometry column
    // restore gap in QgsAttributeMap if geometry column is not last (WORKAROUND)
    QMap<int, int> attrLookup = attributeLookup( offlineLayer, remoteLayer );
    QgsAttributes newAttrs( newAttrsCount );
    QgsAttributes attrs = it->attributes();
    for ( int it = 0; it < attrs.count(); ++it )
    {
      int remoteAttributeIndex = attrLookup[ it ];
      QVariant attr = attrs.at( it );
      if ( remoteLayer->fields().at( remoteAttributeIndex ).type() == QVariant::StringList )
      {
        if ( attr.type() == QVariant::StringList || attr.type() == QVariant::List )
        {
          attr = attr.toStringList();
        }
        else
        {
          attr = QgsJsonUtils::parseArray( attr.toString(), QVariant::String );
        }
      }
      else if ( remoteLayer->fields().at( remoteAttributeIndex ).type() == QVariant::List )
      {
        if ( attr.type() == QVariant::StringList || attr.type() == QVariant::List )
        {
          attr = attr.toList();
        }
        else
        {
          attr = QgsJsonUtils::parseArray( attr.toString(), remoteLayer->fields().at( remoteAttributeIndex ).subType() );
        }
      }
      newAttrs[ remoteAttributeIndex ] = attr;
    }

    // respect constraints and provider default values
    QgsFeature f = QgsVectorLayerUtils::createFeature( remoteLayer, it->geometry(), newAttrs.toMap(), &context );
    remoteLayer->addFeature( f );

    emit progressUpdated( i++ );
  }
}

void QgsOfflineEditing::applyFeaturesRemoved( QgsVectorLayer *remoteLayer, sqlite3 *db, int layerId )
{
  Q_ASSERT( remoteLayer );

  QString sql = QStringLiteral( "SELECT \"fid\" FROM 'log_removed_features' WHERE \"layer_id\" = %1" ).arg( layerId );
  QgsFeatureIds values = sqlQueryFeaturesRemoved( db, sql );

  emit progressModeSet( QgsOfflineEditing::RemoveFeatures, values.size() );

  int i = 1;
  for ( QgsFeatureIds::const_iterator it = values.constBegin(); it != values.constEnd(); ++it )
  {
    QgsFeatureId fid = remoteFid( db, layerId, *it );
    remoteLayer->deleteFeature( fid );

    emit progressUpdated( i++ );
  }
}

void QgsOfflineEditing::applyAttributeValueChanges( QgsVectorLayer *offlineLayer, QgsVectorLayer *remoteLayer, sqlite3 *db, int layerId, int commitNo )
{
  Q_ASSERT( offlineLayer );
  Q_ASSERT( remoteLayer );

  QString sql = QStringLiteral( "SELECT \"fid\", \"attr\", \"value\" FROM 'log_feature_updates' WHERE \"layer_id\" = %1 AND \"commit_no\" = %2 " ).arg( layerId ).arg( commitNo );
  AttributeValueChanges values = sqlQueryAttributeValueChanges( db, sql );

  emit progressModeSet( QgsOfflineEditing::UpdateFeatures, values.size() );

  QMap<int, int> attrLookup = attributeLookup( offlineLayer, remoteLayer );

  for ( int i = 0; i < values.size(); i++ )
  {
    QgsFeatureId fid = remoteFid( db, layerId, values.at( i ).fid );
    QgsDebugMsgLevel( QStringLiteral( "Offline changeAttributeValue %1 = %2" ).arg( attrLookup[ values.at( i ).attr ] ).arg( values.at( i ).value ), 4 );

    int remoteAttributeIndex = attrLookup[ values.at( i ).attr ];
    QVariant attr = values.at( i ).value;
    if ( remoteLayer->fields().at( remoteAttributeIndex ).type() == QVariant::StringList )
    {
      attr = QgsJsonUtils::parseArray( attr.toString(), QVariant::String );
    }
    else if ( remoteLayer->fields().at( remoteAttributeIndex ).type() == QVariant::List )
    {
      attr = QgsJsonUtils::parseArray( attr.toString(), remoteLayer->fields().at( remoteAttributeIndex ).subType() );
    }

    remoteLayer->changeAttributeValue( fid, remoteAttributeIndex, attr );

    emit progressUpdated( i + 1 );
  }
}

void QgsOfflineEditing::applyGeometryChanges( QgsVectorLayer *remoteLayer, sqlite3 *db, int layerId, int commitNo )
{
  Q_ASSERT( remoteLayer );

  QString sql = QStringLiteral( "SELECT \"fid\", \"geom_wkt\" FROM 'log_geometry_updates' WHERE \"layer_id\" = %1 AND \"commit_no\" = %2" ).arg( layerId ).arg( commitNo );
  GeometryChanges values = sqlQueryGeometryChanges( db, sql );

  emit progressModeSet( QgsOfflineEditing::UpdateGeometries, values.size() );

  for ( int i = 0; i < values.size(); i++ )
  {
    QgsFeatureId fid = remoteFid( db, layerId, values.at( i ).fid );
    QgsGeometry newGeom = QgsGeometry::fromWkt( values.at( i ).geom_wkt );
    remoteLayer->changeGeometry( fid, newGeom );

    emit progressUpdated( i + 1 );
  }
}

void QgsOfflineEditing::updateFidLookup( QgsVectorLayer *remoteLayer, sqlite3 *db, int layerId )
{
  Q_ASSERT( remoteLayer );

  // update fid lookup for added features

  // get remote added fids
  // NOTE: use QMap for sorted fids
  QMap < QgsFeatureId, bool /*dummy*/ > newRemoteFids;
  QgsFeature f;

  QgsFeatureIterator fit = remoteLayer->getFeatures( QgsFeatureRequest().setFlags( QgsFeatureRequest::NoGeometry ).setNoAttributes() );

  emit progressModeSet( QgsOfflineEditing::ProcessFeatures, remoteLayer->featureCount() );

  int i = 1;
  while ( fit.nextFeature( f ) )
  {
    if ( offlineFid( db, layerId, f.id() ) == -1 )
    {
      newRemoteFids[ f.id()] = true;
    }

    emit progressUpdated( i++ );
  }

  // get local added fids
  // NOTE: fids are sorted
  QString sql = QStringLiteral( "SELECT \"fid\" FROM 'log_added_features' WHERE \"layer_id\" = %1" ).arg( layerId );
  QList<int> newOfflineFids = sqlQueryInts( db, sql );

  if ( newRemoteFids.size() != newOfflineFids.size() )
  {
    //showWarning( QString( "Different number of new features on offline layer (%1) and remote layer (%2)" ).arg(newOfflineFids.size()).arg(newRemoteFids.size()) );
  }
  else
  {
    // add new fid lookups
    i = 0;
    sqlExec( db, QStringLiteral( "BEGIN" ) );
    for ( QMap<QgsFeatureId, bool>::const_iterator it = newRemoteFids.constBegin(); it != newRemoteFids.constEnd(); ++it )
    {
      addFidLookup( db, layerId, newOfflineFids.at( i++ ), it.key() );
    }
    sqlExec( db, QStringLiteral( "COMMIT" ) );
  }
}

void QgsOfflineEditing::copySymbology( QgsVectorLayer *sourceLayer, QgsVectorLayer *targetLayer )
{
  Q_ASSERT( sourceLayer );
  Q_ASSERT( targetLayer );

  targetLayer->styleManager()->copyStylesFrom( sourceLayer->styleManager() );

  QString error;
  QDomDocument doc;
  QgsReadWriteContext context;
  QgsMapLayer::StyleCategories categories = static_cast<QgsMapLayer::StyleCategories>( QgsMapLayer::AllStyleCategories ) & ~QgsMapLayer::CustomProperties;
  sourceLayer->exportNamedStyle( doc, error, context, categories );

  if ( error.isEmpty() )
  {
    targetLayer->importNamedStyle( doc, error, categories );
  }
  if ( !error.isEmpty() )
  {
    showWarning( error );
  }
}

void QgsOfflineEditing::updateRelations( QgsVectorLayer *sourceLayer, QgsVectorLayer *targetLayer )
{
  Q_ASSERT( sourceLayer );
  Q_ASSERT( targetLayer );

  QgsRelationManager *relationManager = QgsProject::instance()->relationManager();
  const QList<QgsRelation> referencedRelations = relationManager->referencedRelations( sourceLayer );

  for ( QgsRelation relation : referencedRelations )
  {
    relationManager->removeRelation( relation );
    relation.setReferencedLayer( targetLayer->id() );
    relationManager->addRelation( relation );
  }

  const QList<QgsRelation> referencingRelations = relationManager->referencingRelations( sourceLayer );

  for ( QgsRelation relation : referencingRelations )
  {
    relationManager->removeRelation( relation );
    relation.setReferencingLayer( targetLayer->id() );
    relationManager->addRelation( relation );
  }
}

void QgsOfflineEditing::updateMapThemes( QgsVectorLayer *sourceLayer, QgsVectorLayer *targetLayer )
{
  Q_ASSERT( sourceLayer );
  Q_ASSERT( targetLayer );

  QgsMapThemeCollection *mapThemeCollection = QgsProject::instance()->mapThemeCollection();
  const QStringList mapThemeNames = mapThemeCollection->mapThemes();

  for ( const QString &mapThemeName : mapThemeNames )
  {
    QgsMapThemeCollection::MapThemeRecord record = mapThemeCollection->mapThemeState( mapThemeName );

    const auto layerRecords = record.layerRecords();

    for ( QgsMapThemeCollection::MapThemeLayerRecord layerRecord : layerRecords )
    {
      if ( layerRecord.layer() == sourceLayer )
      {
        layerRecord.setLayer( targetLayer );
        record.removeLayerRecord( sourceLayer );
        record.addLayerRecord( layerRecord );
      }
    }

    QgsProject::instance()->mapThemeCollection()->update( mapThemeName, record );
  }
}

void QgsOfflineEditing::updateLayerOrder( QgsVectorLayer *sourceLayer, QgsVectorLayer *targetLayer )
{
  Q_ASSERT( sourceLayer );
  Q_ASSERT( targetLayer );

  QList<QgsMapLayer *>  layerOrder = QgsProject::instance()->layerTreeRoot()->customLayerOrder();

  auto iterator = layerOrder.begin();

  while ( iterator != layerOrder.end() )
  {
    if ( *iterator == targetLayer )
    {
      iterator = layerOrder.erase( iterator );
      if ( iterator == layerOrder.end() )
        break;
    }

    if ( *iterator == sourceLayer )
    {
      *iterator = targetLayer;
    }

    ++iterator;
  }

  QgsProject::instance()->layerTreeRoot()->setCustomLayerOrder( layerOrder );
}

// NOTE: use this to map column indices in case the remote geometry column is not last
QMap<int, int> QgsOfflineEditing::attributeLookup( QgsVectorLayer *offlineLayer, QgsVectorLayer *remoteLayer )
{
  Q_ASSERT( offlineLayer );
  Q_ASSERT( remoteLayer );

  const QgsAttributeList &offlineAttrs = offlineLayer->attributeList();

  QMap < int /*offline attr*/, int /*remote attr*/ > attrLookup;
  // NOTE: though offlineAttrs can have new attributes not yet synced, we take the amount of offlineAttrs
  // because we anyway only add mapping for the fields existing in remoteLayer (this because it could contain fid on 0)
  for ( int i = 0; i < offlineAttrs.size(); i++ )
  {
    if ( remoteLayer->fields().lookupField( offlineLayer->fields().field( i ).name() ) >= 0 )
      attrLookup.insert( offlineAttrs.at( i ), remoteLayer->fields().indexOf( offlineLayer->fields().field( i ).name() ) );
  }

  return attrLookup;
}

void QgsOfflineEditing::showWarning( const QString &message )
{
  emit warning( tr( "Offline Editing Plugin" ), message );
}

sqlite3_database_unique_ptr QgsOfflineEditing::openLoggingDb()
{
  sqlite3_database_unique_ptr database;
  QString dbPath = QgsProject::instance()->readEntry( PROJECT_ENTRY_SCOPE_OFFLINE, PROJECT_ENTRY_KEY_OFFLINE_DB_PATH );
  if ( !dbPath.isEmpty() )
  {
    QString absoluteDbPath = QgsProject::instance()->readPath( dbPath );
    int rc = database.open( absoluteDbPath );
    if ( rc != SQLITE_OK )
    {
      QgsDebugMsg( QStringLiteral( "Could not open the SpatiaLite logging database" ) );
      showWarning( tr( "Could not open the SpatiaLite logging database" ) );
    }
  }
  else
  {
    QgsDebugMsg( QStringLiteral( "dbPath is empty!" ) );
  }
  return database;
}

int QgsOfflineEditing::getOrCreateLayerId( sqlite3 *db, const QString &qgisLayerId )
{
  QString sql = QStringLiteral( "SELECT \"id\" FROM 'log_layer_ids' WHERE \"qgis_id\" = '%1'" ).arg( qgisLayerId );
  int layerId = sqlQueryInt( db, sql, -1 );
  if ( layerId == -1 )
  {
    // next layer id
    sql = QStringLiteral( "SELECT \"last_index\" FROM 'log_indices' WHERE \"name\" = 'layer_id'" );
    int newLayerId = sqlQueryInt( db, sql, -1 );

    // insert layer
    sql = QStringLiteral( "INSERT INTO 'log_layer_ids' VALUES (%1, '%2')" ).arg( newLayerId ).arg( qgisLayerId );
    sqlExec( db, sql );

    // increase layer_id
    // TODO: use trigger for auto increment?
    sql = QStringLiteral( "UPDATE 'log_indices' SET 'last_index' = %1 WHERE \"name\" = 'layer_id'" ).arg( newLayerId + 1 );
    sqlExec( db, sql );

    layerId = newLayerId;
  }

  return layerId;
}

int QgsOfflineEditing::getCommitNo( sqlite3 *db )
{
  QString sql = QStringLiteral( "SELECT \"last_index\" FROM 'log_indices' WHERE \"name\" = 'commit_no'" );
  return sqlQueryInt( db, sql, -1 );
}

void QgsOfflineEditing::increaseCommitNo( sqlite3 *db )
{
  QString sql = QStringLiteral( "UPDATE 'log_indices' SET 'last_index' = %1 WHERE \"name\" = 'commit_no'" ).arg( getCommitNo( db ) + 1 );
  sqlExec( db, sql );
}

void QgsOfflineEditing::addFidLookup( sqlite3 *db, int layerId, QgsFeatureId offlineFid, QgsFeatureId remoteFid )
{
  QString sql = QStringLiteral( "INSERT INTO 'log_fids' VALUES ( %1, %2, %3 )" ).arg( layerId ).arg( offlineFid ).arg( remoteFid );
  sqlExec( db, sql );
}

QgsFeatureId QgsOfflineEditing::remoteFid( sqlite3 *db, int layerId, QgsFeatureId offlineFid )
{
  QString sql = QStringLiteral( "SELECT \"remote_fid\" FROM 'log_fids' WHERE \"layer_id\" = %1 AND \"offline_fid\" = %2" ).arg( layerId ).arg( offlineFid );
  return sqlQueryInt( db, sql, -1 );
}

QgsFeatureId QgsOfflineEditing::offlineFid( sqlite3 *db, int layerId, QgsFeatureId remoteFid )
{
  QString sql = QStringLiteral( "SELECT \"offline_fid\" FROM 'log_fids' WHERE \"layer_id\" = %1 AND \"remote_fid\" = %2" ).arg( layerId ).arg( remoteFid );
  return sqlQueryInt( db, sql, -1 );
}

bool QgsOfflineEditing::isAddedFeature( sqlite3 *db, int layerId, QgsFeatureId fid )
{
  QString sql = QStringLiteral( "SELECT COUNT(\"fid\") FROM 'log_added_features' WHERE \"layer_id\" = %1 AND \"fid\" = %2" ).arg( layerId ).arg( fid );
  return ( sqlQueryInt( db, sql, 0 ) > 0 );
}

int QgsOfflineEditing::sqlExec( sqlite3 *db, const QString &sql )
{
  char *errmsg = nullptr;
  int rc = sqlite3_exec( db, sql.toUtf8(), nullptr, nullptr, &errmsg );
  if ( rc != SQLITE_OK )
  {
    showWarning( errmsg );
  }
  return rc;
}

int QgsOfflineEditing::sqlQueryInt( sqlite3 *db, const QString &sql, int defaultValue )
{
  sqlite3_stmt *stmt = nullptr;
  if ( sqlite3_prepare_v2( db, sql.toUtf8().constData(), -1, &stmt, nullptr ) != SQLITE_OK )
  {
    showWarning( sqlite3_errmsg( db ) );
    return defaultValue;
  }

  int value = defaultValue;
  int ret = sqlite3_step( stmt );
  if ( ret == SQLITE_ROW )
  {
    value = sqlite3_column_int( stmt, 0 );
  }
  sqlite3_finalize( stmt );

  return value;
}

QList<int> QgsOfflineEditing::sqlQueryInts( sqlite3 *db, const QString &sql )
{
  QList<int> values;

  sqlite3_stmt *stmt = nullptr;
  if ( sqlite3_prepare_v2( db, sql.toUtf8().constData(), -1, &stmt, nullptr ) != SQLITE_OK )
  {
    showWarning( sqlite3_errmsg( db ) );
    return values;
  }

  int ret = sqlite3_step( stmt );
  while ( ret == SQLITE_ROW )
  {
    values << sqlite3_column_int( stmt, 0 );

    ret = sqlite3_step( stmt );
  }
  sqlite3_finalize( stmt );

  return values;
}

QList<QgsField> QgsOfflineEditing::sqlQueryAttributesAdded( sqlite3 *db, const QString &sql )
{
  QList<QgsField> values;

  sqlite3_stmt *stmt = nullptr;
  if ( sqlite3_prepare_v2( db, sql.toUtf8().constData(), -1, &stmt, nullptr ) != SQLITE_OK )
  {
    showWarning( sqlite3_errmsg( db ) );
    return values;
  }

  int ret = sqlite3_step( stmt );
  while ( ret == SQLITE_ROW )
  {
    QgsField field( QString( reinterpret_cast< const char * >( sqlite3_column_text( stmt, 0 ) ) ),
                    static_cast< QVariant::Type >( sqlite3_column_int( stmt, 1 ) ),
                    QString(), // typeName
                    sqlite3_column_int( stmt, 2 ),
                    sqlite3_column_int( stmt, 3 ),
                    QString( reinterpret_cast< const char * >( sqlite3_column_text( stmt, 4 ) ) ) );
    values << field;

    ret = sqlite3_step( stmt );
  }
  sqlite3_finalize( stmt );

  return values;
}

QgsFeatureIds QgsOfflineEditing::sqlQueryFeaturesRemoved( sqlite3 *db, const QString &sql )
{
  QgsFeatureIds values;

  sqlite3_stmt *stmt = nullptr;
  if ( sqlite3_prepare_v2( db, sql.toUtf8().constData(), -1, &stmt, nullptr ) != SQLITE_OK )
  {
    showWarning( sqlite3_errmsg( db ) );
    return values;
  }

  int ret = sqlite3_step( stmt );
  while ( ret == SQLITE_ROW )
  {
    values << sqlite3_column_int( stmt, 0 );

    ret = sqlite3_step( stmt );
  }
  sqlite3_finalize( stmt );

  return values;
}

QgsOfflineEditing::AttributeValueChanges QgsOfflineEditing::sqlQueryAttributeValueChanges( sqlite3 *db, const QString &sql )
{
  AttributeValueChanges values;

  sqlite3_stmt *stmt = nullptr;
  if ( sqlite3_prepare_v2( db, sql.toUtf8().constData(), -1, &stmt, nullptr ) != SQLITE_OK )
  {
    showWarning( sqlite3_errmsg( db ) );
    return values;
  }

  int ret = sqlite3_step( stmt );
  while ( ret == SQLITE_ROW )
  {
    AttributeValueChange change;
    change.fid = sqlite3_column_int( stmt, 0 );
    change.attr = sqlite3_column_int( stmt, 1 );
    change.value = QString( reinterpret_cast< const char * >( sqlite3_column_text( stmt, 2 ) ) );
    values << change;

    ret = sqlite3_step( stmt );
  }
  sqlite3_finalize( stmt );

  return values;
}

QgsOfflineEditing::GeometryChanges QgsOfflineEditing::sqlQueryGeometryChanges( sqlite3 *db, const QString &sql )
{
  GeometryChanges values;

  sqlite3_stmt *stmt = nullptr;
  if ( sqlite3_prepare_v2( db, sql.toUtf8().constData(), -1, &stmt, nullptr ) != SQLITE_OK )
  {
    showWarning( sqlite3_errmsg( db ) );
    return values;
  }

  int ret = sqlite3_step( stmt );
  while ( ret == SQLITE_ROW )
  {
    GeometryChange change;
    change.fid = sqlite3_column_int( stmt, 0 );
    change.geom_wkt = QString( reinterpret_cast< const char * >( sqlite3_column_text( stmt, 1 ) ) );
    values << change;

    ret = sqlite3_step( stmt );
  }
  sqlite3_finalize( stmt );

  return values;
}

void QgsOfflineEditing::committedAttributesAdded( const QString &qgisLayerId, const QList<QgsField> &addedAttributes )
{
  sqlite3_database_unique_ptr database = openLoggingDb();
  if ( !database )
    return;

  // insert log
  int layerId = getOrCreateLayerId( database.get(), qgisLayerId );
  int commitNo = getCommitNo( database.get() );

  for ( const QgsField &field : addedAttributes )
  {
    QString sql = QStringLiteral( "INSERT INTO 'log_added_attrs' VALUES ( %1, %2, '%3', %4, %5, %6, '%7' )" )
                  .arg( layerId )
                  .arg( commitNo )
                  .arg( field.name() )
                  .arg( field.type() )
                  .arg( field.length() )
                  .arg( field.precision() )
                  .arg( field.comment() );
    sqlExec( database.get(), sql );
  }

  increaseCommitNo( database.get() );
}

void QgsOfflineEditing::committedFeaturesAdded( const QString &qgisLayerId, const QgsFeatureList &addedFeatures )
{
  sqlite3_database_unique_ptr database = openLoggingDb();
  if ( !database )
    return;

  // insert log
  int layerId = getOrCreateLayerId( database.get(), qgisLayerId );

  // get new feature ids from db
  QgsMapLayer *layer = QgsProject::instance()->mapLayer( qgisLayerId );
  QString dataSourceString = layer->source();
  QgsDataSourceUri uri = QgsDataSourceUri( dataSourceString );

  QString offlinePath = QgsProject::instance()->readPath( QgsProject::instance()->readEntry( PROJECT_ENTRY_SCOPE_OFFLINE, PROJECT_ENTRY_KEY_OFFLINE_DB_PATH ) );
  QString tableName;

  if ( !offlinePath.contains( ".gpkg" ) )
  {
    tableName = uri.table();
  }
  else
  {
    QgsProviderMetadata *ogrProviderMetaData = QgsProviderRegistry::instance()->providerMetadata( QStringLiteral( "ogr" ) );
    QVariantMap decodedUri = ogrProviderMetaData->decodeUri( dataSourceString );
    tableName = decodedUri.value( QStringLiteral( "layerName" ) ).toString();
    if ( tableName.isEmpty() )
    {
      showWarning( tr( "Could not deduce table name from data source %1." ).arg( dataSourceString ) );
    }
  }

  // only store feature ids
  QString sql = QStringLiteral( "SELECT ROWID FROM '%1' ORDER BY ROWID DESC LIMIT %2" ).arg( tableName ).arg( addedFeatures.size() );
  QList<int> newFeatureIds = sqlQueryInts( database.get(), sql );
  for ( int i = newFeatureIds.size() - 1; i >= 0; i-- )
  {
    QString sql = QStringLiteral( "INSERT INTO 'log_added_features' VALUES ( %1, %2 )" )
                  .arg( layerId )
                  .arg( newFeatureIds.at( i ) );
    sqlExec( database.get(), sql );
  }
}

void QgsOfflineEditing::committedFeaturesRemoved( const QString &qgisLayerId, const QgsFeatureIds &deletedFeatureIds )
{
  sqlite3_database_unique_ptr database = openLoggingDb();
  if ( !database )
    return;

  // insert log
  int layerId = getOrCreateLayerId( database.get(), qgisLayerId );

  for ( QgsFeatureId id : deletedFeatureIds )
  {
    if ( isAddedFeature( database.get(), layerId, id ) )
    {
      // remove from added features log
      QString sql = QStringLiteral( "DELETE FROM 'log_added_features' WHERE \"layer_id\" = %1 AND \"fid\" = %2" ).arg( layerId ).arg( id );
      sqlExec( database.get(), sql );
    }
    else
    {
      QString sql = QStringLiteral( "INSERT INTO 'log_removed_features' VALUES ( %1, %2)" )
                    .arg( layerId )
                    .arg( id );
      sqlExec( database.get(), sql );
    }
  }
}

void QgsOfflineEditing::committedAttributeValuesChanges( const QString &qgisLayerId, const QgsChangedAttributesMap &changedAttrsMap )
{
  sqlite3_database_unique_ptr database = openLoggingDb();
  if ( !database )
    return;

  // insert log
  int layerId = getOrCreateLayerId( database.get(), qgisLayerId );
  int commitNo = getCommitNo( database.get() );

  for ( QgsChangedAttributesMap::const_iterator cit = changedAttrsMap.begin(); cit != changedAttrsMap.end(); ++cit )
  {
    QgsFeatureId fid = cit.key();
    if ( isAddedFeature( database.get(), layerId, fid ) )
    {
      // skip added features
      continue;
    }
    QgsAttributeMap attrMap = cit.value();
    for ( QgsAttributeMap::const_iterator it = attrMap.constBegin(); it != attrMap.constEnd(); ++it )
    {
      QString value = it.value().type() == QVariant::StringList || it.value().type() == QVariant::List ? QgsJsonUtils::encodeValue( it.value() ) : it.value().toString();
      value.replace( QLatin1String( "'" ), QLatin1String( "''" ) ); // escape quote
      QString sql = QStringLiteral( "INSERT INTO 'log_feature_updates' VALUES ( %1, %2, %3, %4, '%5' )" )
                    .arg( layerId )
                    .arg( commitNo )
                    .arg( fid )
                    .arg( it.key() ) // attribute
                    .arg( value );
      sqlExec( database.get(), sql );
    }
  }

  increaseCommitNo( database.get() );
}

void QgsOfflineEditing::committedGeometriesChanges( const QString &qgisLayerId, const QgsGeometryMap &changedGeometries )
{
  sqlite3_database_unique_ptr database = openLoggingDb();
  if ( !database )
    return;

  // insert log
  int layerId = getOrCreateLayerId( database.get(), qgisLayerId );
  int commitNo = getCommitNo( database.get() );

  for ( QgsGeometryMap::const_iterator it = changedGeometries.begin(); it != changedGeometries.end(); ++it )
  {
    QgsFeatureId fid = it.key();
    if ( isAddedFeature( database.get(), layerId, fid ) )
    {
      // skip added features
      continue;
    }
    QgsGeometry geom = it.value();
    QString sql = QStringLiteral( "INSERT INTO 'log_geometry_updates' VALUES ( %1, %2, %3, '%4' )" )
                  .arg( layerId )
                  .arg( commitNo )
                  .arg( fid )
                  .arg( geom.asWkt() );
    sqlExec( database.get(), sql );

    // TODO: use WKB instead of WKT?
  }

  increaseCommitNo( database.get() );
}

void QgsOfflineEditing::startListenFeatureChanges()
{
  QgsVectorLayer *vLayer = qobject_cast<QgsVectorLayer *>( sender() );

  Q_ASSERT( vLayer );

  // enable logging, check if editBuffer is not null
  if ( vLayer->editBuffer() )
  {
    QgsVectorLayerEditBuffer *editBuffer = vLayer->editBuffer();
    connect( editBuffer, &QgsVectorLayerEditBuffer::committedAttributesAdded,
             this, &QgsOfflineEditing::committedAttributesAdded );
    connect( editBuffer, &QgsVectorLayerEditBuffer::committedAttributeValuesChanges,
             this, &QgsOfflineEditing::committedAttributeValuesChanges );
    connect( editBuffer, &QgsVectorLayerEditBuffer::committedGeometriesChanges,
             this, &QgsOfflineEditing::committedGeometriesChanges );
  }
  connect( vLayer, &QgsVectorLayer::committedFeaturesAdded,
           this, &QgsOfflineEditing::committedFeaturesAdded );
  connect( vLayer, &QgsVectorLayer::committedFeaturesRemoved,
           this, &QgsOfflineEditing::committedFeaturesRemoved );
}

void QgsOfflineEditing::stopListenFeatureChanges()
{
  QgsVectorLayer *vLayer = qobject_cast<QgsVectorLayer *>( sender() );

  Q_ASSERT( vLayer );

  // disable logging, check if editBuffer is not null
  if ( vLayer->editBuffer() )
  {
    QgsVectorLayerEditBuffer *editBuffer = vLayer->editBuffer();
    disconnect( editBuffer, &QgsVectorLayerEditBuffer::committedAttributesAdded,
                this, &QgsOfflineEditing::committedAttributesAdded );
    disconnect( editBuffer, &QgsVectorLayerEditBuffer::committedAttributeValuesChanges,
                this, &QgsOfflineEditing::committedAttributeValuesChanges );
    disconnect( editBuffer, &QgsVectorLayerEditBuffer::committedGeometriesChanges,
                this, &QgsOfflineEditing::committedGeometriesChanges );
  }
  disconnect( vLayer, &QgsVectorLayer::committedFeaturesAdded,
              this, &QgsOfflineEditing::committedFeaturesAdded );
  disconnect( vLayer, &QgsVectorLayer::committedFeaturesRemoved,
              this, &QgsOfflineEditing::committedFeaturesRemoved );
}

void QgsOfflineEditing::layerAdded( QgsMapLayer *layer )
{
  Q_ASSERT( layer );

  // detect offline layer
  if ( layer->customProperty( CUSTOM_PROPERTY_IS_OFFLINE_EDITABLE, false ).toBool() )
  {
    QgsVectorLayer *vLayer = qobject_cast<QgsVectorLayer *>( layer );

    Q_ASSERT( vLayer );

    connect( vLayer, &QgsVectorLayer::editingStarted, this, &QgsOfflineEditing::startListenFeatureChanges );
    connect( vLayer, &QgsVectorLayer::editingStopped, this, &QgsOfflineEditing::stopListenFeatureChanges );
  }
}

