/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "MainWindow.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QComboBox>
#include <QPushButton>
#include <QSlider>
#include <QRadioButton>
#include <QButtonGroup>
#include <QColorDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QInputDialog>
#include <QUuid>

namespace ucc
{

void MainWindow::connectKeyboardBacklightPageWidgets()
{
  connect( m_keyboardBrightnessSlider, &QSlider::valueChanged,
           this, &MainWindow::onKeyboardBrightnessChanged );

  connect( m_keyboardColorButton, &QPushButton::clicked,
           this, &MainWindow::onKeyboardColorClicked );

  connect( m_keyboardVisualizer, &KeyboardVisualizerWidget::colorsChanged,
           this, &MainWindow::onKeyboardVisualizerColorsChanged );

  connect( m_keyboardProfileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
           this, [this]( int index ) {
    if ( index >= 0 )
      onKeyboardProfileChanged( m_keyboardProfileCombo->itemData( index ).toString() );
  } );

  connect( m_keyboardProfileCombo->lineEdit(), &QLineEdit::editingFinished,
           this, &MainWindow::onKeyboardProfileComboRenamed );

  connect( m_copyKeyboardProfileButton, &QPushButton::clicked,
           this, &MainWindow::onCopyKeyboardProfileClicked );

  connect( m_saveKeyboardProfileButton, &QPushButton::clicked,
           this, &MainWindow::onSaveKeyboardProfileClicked );

  connect( m_removeKeyboardProfileButton, &QPushButton::clicked,
           this, &MainWindow::onRemoveKeyboardProfileClicked );
}

void MainWindow::setupKeyboardBacklightPage()
{
  QGroupBox *keyboardWidget = new QGroupBox( "Keyboard Controls" );
  QVBoxLayout *mainLayout = new QVBoxLayout( keyboardWidget );

  // Keyboard profile controls
  QHBoxLayout *profileLayout = new QHBoxLayout();
  profileLayout->setContentsMargins( 5, 5, 5, 5 );
  profileLayout->setSpacing( 0 );
  QLabel *profileLabel = new QLabel( "Keyboard Profile:" );
  m_keyboardProfileCombo = new QComboBox();
  m_keyboardProfileCombo->setEditable( true );
  m_keyboardProfileCombo->setInsertPolicy( QComboBox::NoInsert );

  // Add custom keyboard profiles from settings
  for ( const auto &v : m_profileManager->customKeyboardProfilesData() )
  {
    QJsonObject o = v.toObject();
    m_keyboardProfileCombo->addItem( o["name"].toString(), o["id"].toString() );
  }

  // If no custom profiles exist, create a "Current" profile from hardware state
  if ( m_keyboardProfileCombo->count() == 0 )
  {
    QString currentId = QUuid::createUuid().toString( QUuid::WithoutBraces );
    QString json;

    // Read live state from hardware
    if ( auto states = m_UccdClient->getKeyboardBacklightStates() )
    {
      QJsonDocument statesDoc = QJsonDocument::fromJson( QString::fromStdString( *states ).toUtf8() );
      if ( statesDoc.isArray() )
      {
        QJsonObject wrapper;
        wrapper["states"] = statesDoc.array();
        // Extract brightness from first zone
        if ( !statesDoc.array().isEmpty() )
          wrapper["brightness"] = statesDoc.array()[0].toObject()["brightness"].toInt( 50 );
        json = QJsonDocument( wrapper ).toJson( QJsonDocument::Compact );
      }
    }

    if ( json.isEmpty() )
      json = QStringLiteral( "{\"brightness\":50,\"states\":[]}" );

    if ( m_profileManager->setKeyboardProfile( currentId, "Current", json ) )
    {
      m_keyboardProfileCombo->addItem( "Current", currentId );
      qDebug() << "[KBD INIT] Auto-saved 'Current' keyboard profile:" << currentId;
    }
  }
  
  m_copyKeyboardProfileButton = new QPushButton("Copy");  
  m_saveKeyboardProfileButton = new QPushButton("Save");
  m_removeKeyboardProfileButton = new QPushButton("Remove");

  profileLayout->addWidget( profileLabel );
  profileLayout->addWidget( m_keyboardProfileCombo, 1 );
  profileLayout->addWidget( m_copyKeyboardProfileButton );
  profileLayout->addWidget( m_saveKeyboardProfileButton );
  profileLayout->addWidget( m_removeKeyboardProfileButton );
  profileLayout->addStretch();
  mainLayout->addLayout( profileLayout );

  // Add a separator line
  QFrame *separator = new QFrame();
  separator->setFrameShape( QFrame::HLine );
  separator->setStyleSheet( "color: #cccccc;" );
  mainLayout->addWidget( separator );

  // Check if keyboard backlight is supported
  if ( auto info = m_UccdClient->getKeyboardBacklightInfo() )
  {
    // Parse the JSON to get capabilities
    if ( QJsonDocument doc = QJsonDocument::fromJson( QString::fromStdString( *info ).toUtf8() ); doc.isObject() )
    {
      QJsonObject caps = doc.object();
      int zones = caps["zones"].toInt();
      int maxBrightness = caps["maxBrightness"].toInt();
      int maxRed = caps["maxRed"].toInt();
      int maxGreen = caps["maxGreen"].toInt();
      int maxBlue = caps["maxBlue"].toInt();

      if ( zones > 0 )
      {
        QHBoxLayout *brightnessLayout = new QHBoxLayout();
        brightnessLayout->setContentsMargins( 5, 5, 5, 5 );
        brightnessLayout->setSpacing( 0 );

        QLabel *brightnessLabel = new QLabel( "Brightness:" );
        m_keyboardBrightnessSlider = new QSlider( Qt::Horizontal );
        m_keyboardBrightnessSlider->setMinimum( 0 );
        m_keyboardBrightnessSlider->setMaximum( maxBrightness );
        m_keyboardBrightnessSlider->setValue( maxBrightness );
        m_keyboardBrightnessValueLabel = new QLabel( QString::number( maxBrightness ) );
        m_keyboardBrightnessValueLabel->setMinimumWidth( 40 );

        brightnessLayout->addWidget( brightnessLabel );
        brightnessLayout->addWidget( m_keyboardBrightnessSlider );
        brightnessLayout->addWidget( m_keyboardBrightnessValueLabel );

        // Global color controls for RGB keyboards
        if ( maxRed > 0 && maxGreen > 0 && maxBlue > 0 )
        {
          m_keyboardColorLabel = new QLabel( "Color:" );
          m_keyboardColorButton = new QPushButton( "Choose Color" );
          brightnessLayout->addWidget( m_keyboardColorLabel );
          brightnessLayout->addWidget( m_keyboardColorButton );
        }

        mainLayout->addLayout( brightnessLayout );

        // Keyboard visualizer
        if ( zones > 1 )
        {
          m_keyboardVisualizer = new KeyboardVisualizerWidget( zones, maxBrightness, keyboardWidget );
          mainLayout->addWidget( m_keyboardVisualizer );
        }
      }
      else
      {
        QLabel *noSupportLabel = new QLabel( "Keyboard backlight not supported on this device." );
        mainLayout->addWidget( noSupportLabel );
      }
    }
  }
  else
  {
    QLabel *noSupportLabel = new QLabel( "Keyboard backlight not available." );
    mainLayout->addWidget( noSupportLabel );
  }

  const int tabIndex = m_tabs->addTab( keyboardWidget, "Keyboard and Hardware" );
  m_hardwareTab = new HardwareTab( m_systemMonitor.get(), m_tabs->widget( tabIndex ) );
}

void MainWindow::reloadKeyboardProfiles()
{
  if ( m_keyboardProfileCombo )
  {
    // Block signals during combo rebuild to prevent spurious profile loads
    m_keyboardProfileCombo->blockSignals( true );

    m_keyboardProfileCombo->clear();
    for ( const auto &v : m_profileManager->customKeyboardProfilesData() )
    {
      QJsonObject o = v.toObject();
      m_keyboardProfileCombo->addItem( o["name"].toString(), o["id"].toString() );
    }

    m_keyboardProfileCombo->blockSignals( false );
  }

  updateKeyboardProfileButtonStates();
}

void MainWindow::updateKeyboardProfileButtonStates()
{
  if ( not m_keyboardProfileCombo or not m_copyKeyboardProfileButton or not m_removeKeyboardProfileButton )
    return;

  bool hasProfile = ( m_keyboardProfileCombo->count() > 0 );
  bool canRemove = ( m_keyboardProfileCombo->count() > 1 ); // Keep at least one profile

  m_copyKeyboardProfileButton->setEnabled( hasProfile );
  m_saveKeyboardProfileButton->setEnabled( hasProfile );
  m_removeKeyboardProfileButton->setEnabled( canRemove );
}

void MainWindow::onKeyboardBrightnessChanged( int value )
{
  m_keyboardBrightnessValueLabel->setText( QString::number( value ) );

  if ( m_initializing )
    return;

  // Update visualizer preview — block colorsChanged signal to avoid double hardware write
  if ( m_keyboardVisualizer )
  {
    m_keyboardVisualizer->blockSignals( true );
    m_keyboardVisualizer->setGlobalBrightness( value );
    m_keyboardVisualizer->blockSignals( false );

    // Apply to hardware immediately (same as color changes)
    QJsonArray statesArray = m_keyboardVisualizer->getJSONState();
    if ( !statesArray.empty() )
    {
      QJsonDocument doc( statesArray );
      m_UccdClient->setKeyboardBacklight( doc.toJson( QJsonDocument::Compact ).toStdString() );
    }
  }
}

void MainWindow::onKeyboardColorClicked()
{
  // Open color dialog
  QColor color = QColorDialog::getColor( Qt::white, this, "Choose Keyboard Color" );
  if ( color.isValid() )
  {
    // Update visualizer if it exists
    if ( m_keyboardVisualizer )
      m_keyboardVisualizer->setGlobalColor( color );
    else
    {
      // Fallback for single zone keyboards
      int brightness = m_keyboardBrightnessSlider->value();

      QJsonArray statesArray;
      QJsonObject state;
      state["mode"] = 0; // Static
      state["brightness"] = brightness;
      state["red"] = color.red();
      state["green"] = color.green();
      state["blue"] = color.blue();
      statesArray.append( state );

      QJsonDocument doc( statesArray );
      QString json = doc.toJson( QJsonDocument::Compact );

      if ( not m_UccdClient->setKeyboardBacklight( json.toStdString() ) )
      {
        statusBar()->showMessage( "Failed to set keyboard backlight", 3000 );
      }
    }
  }
}

void MainWindow::onKeyboardVisualizerColorsChanged()
{
  if ( m_initializing )
    return;

  if ( not m_keyboardVisualizer )
    return;

  // Get the color data from the visualizer
  QJsonArray statesArray = m_keyboardVisualizer->getJSONState();
  if ( statesArray.empty() )
    return;

  QJsonDocument doc( statesArray );
  QString json = doc.toJson( QJsonDocument::Compact );

  if ( not m_UccdClient->setKeyboardBacklight( json.toStdString() ) )
  {
    statusBar()->showMessage( "Failed to set keyboard backlight", 3000 );
  }
}

void MainWindow::onKeyboardProfileChanged(const QString& profileId)
{
  if ( profileId.isEmpty() )
    return;

  // Get the keyboard profile data by ID
  QString json = m_profileManager->getKeyboardProfile( profileId );
  qDebug() << "[KBD PROFILE] loading profile:" << profileId << "json length:" << json.size();
  if ( json.isEmpty() or json == "{}" )
  {
    qDebug() << "No keyboard profile data for" << profileId;
    return;
  }

  QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  
  // Parse as object first to check for top-level brightness
  int brightness = -1;
  QJsonArray statesArray;
  
  if ( doc.isObject() )
  {
    QJsonObject obj = doc.object();
    
    // Check for top-level brightness (new format)
    if ( obj.contains( "brightness" ) )
    {
      brightness = obj["brightness"].toInt( -1 );
    }
    
    // Get states array
    if ( obj.contains( "states" ) && obj["states"].isArray() )
    {
      statesArray = obj["states"].toArray();
    }
  }
  else if ( doc.isArray() )
  {
    statesArray = doc.array();
  }
  
  // Apply colors to keyboard visualizer if available
  if ( !statesArray.isEmpty() && m_keyboardVisualizer )
  {
    m_keyboardVisualizer->updateFromJSON( statesArray );
  }
  else if ( !statesArray.isEmpty() )
  {
    // Apply directly to hardware
    if ( not m_UccdClient->setKeyboardBacklight( json.toStdString() ) )
    {
      statusBar()->showMessage( "Failed to load keyboard profile", 3000 );
    }
  }

  // Update brightness slider and visualizer
  if ( brightness < 0 && !statesArray.isEmpty() && statesArray[0].isObject() )
  {
    // Fallback: extract brightness from first state (old format)
    QJsonObject firstState = statesArray[0].toObject();
    brightness = firstState["brightness"].toInt( 128 );
  }
  
  if ( brightness >= 0 )
  {
    qDebug() << "[KBD PROFILE] applying brightness:" << brightness
             << "slider max:" << ( m_keyboardBrightnessSlider ? m_keyboardBrightnessSlider->maximum() : -1 );
    if ( m_keyboardBrightnessSlider )
    {
      m_keyboardBrightnessSlider->blockSignals( true );
      m_keyboardBrightnessSlider->setValue( brightness );
      m_keyboardBrightnessSlider->blockSignals( false );
      m_keyboardBrightnessValueLabel->setText( QString::number( brightness ) );
    }
    if ( m_keyboardVisualizer )
    {
      m_keyboardVisualizer->blockSignals( true );
      m_keyboardVisualizer->setGlobalBrightness( brightness );
      m_keyboardVisualizer->blockSignals( false );
    }
  }

  // Update button states
  updateKeyboardProfileButtonStates();
}

void MainWindow::onCopyKeyboardProfileClicked()
{
  QString currentId = m_keyboardProfileCombo->currentData().toString();
  QString currentName = m_keyboardProfileCombo->currentText();

  // Strip " [Built-in]" suffix if present
  if ( currentName.endsWith(" [Built-in]") )
    currentName = currentName.left( currentName.size() - 11 ); // 11 is length of " [Built-in]"

  // Generate new name: "New {name}" with optional incrementing number
  QString baseName = QString("New %1").arg(currentName);
  QString name = baseName;
  int counter = 1;
  while ( m_keyboardProfileCombo->findText( name ) != -1 ) {
    name = QString("%1 %2").arg(baseName).arg(counter);
    counter++;
  }

  // Get the profile data: for "Current" (empty ID), read the live keyboard state
  QString json;
  if ( currentId.isEmpty() )
  {
    if ( m_keyboardVisualizer )
    {
      QJsonObject wrapper;
      if ( m_keyboardBrightnessSlider )
        wrapper["brightness"] = m_keyboardBrightnessSlider->value();
      wrapper["states"] = m_keyboardVisualizer->getJSONState();
      json = QJsonDocument( wrapper ).toJson( QJsonDocument::Compact );
    }
    else if ( auto states = m_UccdClient->getKeyboardBacklightStates() )
    {
      // Wrap with brightness if available
      QJsonObject wrapper;
      if ( m_keyboardBrightnessSlider )
        wrapper["brightness"] = m_keyboardBrightnessSlider->value();
      QJsonDocument statesDoc = QJsonDocument::fromJson( QString::fromStdString( *states ).toUtf8() );
      if ( statesDoc.isArray() )
        wrapper["states"] = statesDoc.array();
      json = QJsonDocument( wrapper ).toJson( QJsonDocument::Compact );
    }
  }
  else
    json = m_profileManager->getKeyboardProfile( currentId );

  QString newId = QUuid::createUuid().toString( QUuid::WithoutBraces );
  if ( not json.isEmpty() and m_profileManager->setKeyboardProfile( newId, name, json ) )
  {
    // setKeyboardProfile emits customKeyboardProfilesChanged which triggers
    // reloadKeyboardProfiles(), so the combo is already rebuilt. Just select the new item.
    int newIdx = m_keyboardProfileCombo->findData( newId );
    if ( newIdx >= 0 )
      m_keyboardProfileCombo->setCurrentIndex( newIdx );
    statusBar()->showMessage( QString("Keyboard profile '%1' copied to '%2'").arg(currentName, name) );
    updateKeyboardProfileButtonStates();
  }
  else
  {
    QMessageBox::warning( this, "Copy Failed", "Failed to copy keyboard profile." );
  }
}

void MainWindow::onSaveKeyboardProfileClicked()
{
  QString currentId = m_keyboardProfileCombo->currentData().toString();
  QString currentName = m_keyboardProfileCombo->currentText();
  QString json;

  if ( currentId.isEmpty() )
    return;

  // Get current keyboard state (wrapped with brightness)
  if ( m_keyboardVisualizer )
  {
    QJsonObject wrapper;
    if ( m_keyboardBrightnessSlider )
      wrapper["brightness"] = m_keyboardBrightnessSlider->value();
    wrapper["states"] = m_keyboardVisualizer->getJSONState();
    json = QJsonDocument( wrapper ).toJson( QJsonDocument::Compact );
  }
  else if ( auto states = m_UccdClient->getKeyboardBacklightStates() )
  {
    QJsonObject wrapper;
    if ( m_keyboardBrightnessSlider )
      wrapper["brightness"] = m_keyboardBrightnessSlider->value();
    QJsonDocument statesDoc = QJsonDocument::fromJson( QString::fromStdString( *states ).toUtf8() );
    if ( statesDoc.isArray() )
      wrapper["states"] = statesDoc.array();
    json = QJsonDocument( wrapper ).toJson( QJsonDocument::Compact );
  }

  if ( json.isEmpty() )
  {
    QMessageBox::warning( this, "Save Failed", "Unable to get current keyboard state." );
    return;
  }

  if ( m_profileManager->setKeyboardProfile( currentId, currentName, json ) )
    statusBar()->showMessage( QString("Keyboard profile '%1' saved").arg(currentName) );
  else
    QMessageBox::warning( this, "Save Failed", "Failed to save keyboard profile." );
}

void MainWindow::onRemoveKeyboardProfileClicked()
{
  QString currentId = m_keyboardProfileCombo->currentData().toString();
  QString currentName = m_keyboardProfileCombo->currentText();
  
  // Confirm deletion
  QMessageBox::StandardButton reply = QMessageBox::question(
    this, "Remove Keyboard Profile",
    QString("Are you sure you want to remove the keyboard profile '%1'?").arg(currentName),
    QMessageBox::Yes | QMessageBox::No
  );
  
  if ( reply == QMessageBox::Yes )
  {
    // Remove from persistent storage and UI
    if ( not m_profileManager->deleteKeyboardProfile( currentId ) )
      QMessageBox::warning(this, "Remove Failed", "Failed to remove custom keyboard profile.");
    else
    {
      if ( int idx = m_keyboardProfileCombo->currentIndex(); idx >= 0 )
        m_keyboardProfileCombo->removeItem( idx );

      statusBar()->showMessage( QString("Keyboard profile '%1' removed").arg(currentName) );
      updateKeyboardProfileButtonStates();
    }
  }
}

} // namespace ucc