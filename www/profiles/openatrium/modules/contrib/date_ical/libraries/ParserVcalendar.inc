<?php
/**
 * @file
 * Defines a class that parses iCalcreator vcalendar objects into
 * Feeds-compatible data arrays.
 */

class ParserVcalendar {
  /**
   * Variables used for parsing.
   */
  protected $calendar;
  protected $source;
  protected $fetcherResult;
  protected $config;
  protected $timezones = array();
  protected $xtimezone;
  
  /**
   * Variables used for batch processing.
   */
  protected $totalComponents = 0;
  protected $lastComponentParsed = 0;
  
  /**
   * Constructor.
   */
  public function __construct($calendar, $source, $fetcher_result, $config) {
    $this->calendar = $calendar;
    $this->source = $source;
    $this->fetcherResult = $fetcher_result;
    $this->config = $config;
  }

  /**
   * Parses the vcalendar object into an array of event data arrays.
   *
   * @param int $offset
   *   Specifies which section of the feed to start parsing at.
   *
   * @param int $limit
   *   Specifies how many components to parse on this run.
   *
   * @return array
   *   An array of parsed event data keyed by the same strings as the array
   *   returned by DateiCalFeedsParser::getiCalMappingSources().
   */
  public function parse($offset, $limit) {
    // Sometimes, the feed will set a timezone for every event in the calendar
    // using the non-standard X-WR-TIMEZONE property. Date iCal uses this
    // timezone only if the date property is not in UTC and has no TZID.
    $xtimezone = $this->calendar->getProperty('X-WR-TIMEZONE');
    if (!empty($xtimezone[1])) {
      // Allow modules to alter the timezone string before it gets converted
      // into a DateTimeZone.
      $context = array(
        'property_key' => NULL,
        'calendar_component' => NULL,
        'calendar' => $this->calendar,
        'feeeds_source' => $this->source,
        'feeds_fetcher_result' => $this->fetcherResult,
      );
      drupal_alter('date_ical_import_timezone', $xtimezone[1], $context);
      $this->xtimezone = $this->_tzid_to_datetimezone($xtimezone[1]);
    }

    // Collect the timezones into an array, for easier access.
    while ($component = $this->calendar->getComponent('VTIMEZONE')) {
      $this->timezones[] = $component;
    }
    
    // This content array is used by date_ical_import_component_alter() and
    // date_ical_import_parsed_data_alter().
    $context2 = array(
      'calendar' => $this->calendar,
      'source' => $this->source,
      'fetcher_result' => $this->fetcherResult,
    );
    
    // Collect each component, so we can batch them properly in the next loop.
    $raw_components = array();
    $types = array('VEVENT', 'VTODO', 'VJOURNAL', 'VFREEBUSY', 'VALARM');
    foreach ($types as $type) {
      while ($vcalendar_component = $this->calendar->getComponent($type)) {
        // Allow modules to alter the vcalendar component before we parse it
        // into a Feeds-compatible data array.
        drupal_alter('date_ical_import_component', $vcalendar_component, $context2);
        $raw_components[] = $vcalendar_component;
      }
    }
    
    // Store this for use by DateiCalFeedsParser's batch processing code.
    $this->totalComponents = count($raw_components);
    
    // Parse each raw component in the current batch into a Feeds-compatible
    // event data array.
    $events = array();
    $sources = DateiCalFeedsParser::getiCalMappingSources();
    $batch = array_slice($raw_components, $offset, $limit, TRUE);
    foreach ($batch as $ndx => $raw_component) {
      $parsed_component = array();
      foreach ($sources as $property_key => $data) {
        $handler = $data['date_ical_parse_handler'];
        $parsed_component[$property_key] = $this->$handler($property_key, $raw_component);
      }
      // Allow modules to alter the final parsed data before we send it to the
      // Feeds processor.
      drupal_alter('date_ical_import_post_parse', $parsed_component, $context2);
      
      $events[] = $parsed_component;
      // The indices of the original $raw_components array are preserved in
      // $batch, so using the $ndx value here lets us communicate our progress
      // through the full collection of components.
      $this->lastComponentParsed = $ndx;
    }
    
    return $events;
  }
  
  /**
   * Getter for the protected totalComponents property.
   */
  public function getTotalComponents() {
    return $this->totalComponents;
  }
  
  /**
   * Getter for the protected lastComponentParsed property.
   */
  public function getLastComponentParsed() {
    return $this->lastComponentParsed;
  }
  
  /**
   * Parses text fields.
   *
   * @return string
   *   The parsed text property.
   */
  public function parseTextProperty($property_key, $vcalendar_component) {
    $text = $vcalendar_component->getProperty($property_key);
    if ($text === FALSE) {
      if ($property_key == 'SUMMARY') {
        $uid = $vcalendar_component->getProperty('UID');
        throw new DateIcalParseException(t('The component with UID %uid is invalid because it has no SUMMARY (nodes require a title).', array('%uid' => $uid)));
      }
      // If the component doesn't have this property, return NULL.
      return NULL;
    }
    // Convert literal \n and \N into newline characters.
    $text = str_replace(array('\n', '\N'), "\n", $text);
    return $text;
  }

  /**
   * Parses field parameters.
   *
   * @return string
   *   The parsed field parameter.
   */
  public function parsePropertyParameter($property_key, $vcalendar_component) {
    list($key, $attr) = explode(':', $property_key);
    $property = $vcalendar_component->getProperty($key, FALSE, TRUE);
    if ($property === FALSE) {
      // If the component doesn't have this property, return NULL.
      return NULL;
    }
    return isset($property['params'][$attr]) ? $property['params'][$attr] : '';
  }

  /**
   * Parses DATE-TIME and DATE fields.
   *
   * @return FeedsDateTime
   *   The parsed datetime object.
   */
  public function parseDateTimeProperty($property_key, $vcalendar_component) {
    $property = $vcalendar_component->getProperty($property_key, FALSE, TRUE);
    // Gather all the other date properties, so we can work with them later.
    $duration = $vcalendar_component->getProperty('DURATION', FALSE, TRUE);
    $dtstart = $vcalendar_component->getProperty('DTSTART', FALSE, TRUE);
    $uid = $vcalendar_component->getProperty('UID');
    
    // DATE-type properties are treated as All Day events which can span over
    // multiple days.
    // The Date module's All Day event handling was never finalized
    // (http://drupal.org/node/874322), which requires us to do some some
    // special coddling later.
    $is_all_day = (isset($property['params']['VALUE']) && $property['params']['VALUE'] == 'DATE');
    
    // Cover various conditions in which either DTSTART or DTEND are not set.
    if ($property === FALSE) {
      // When DTEND isn't defined, we may need to emulate it.
      if ($property_key == 'DTEND') {
        // Unset DTENDs need to emulate the DATE type from DTSTART.
        $is_all_day = (isset($dtstart['params']['VALUE']) && $dtstart['params']['VALUE'] == 'DATE');
        
        if ($duration !== FALSE) {
          // If a DURATION is defined, emulate DTEND as DTSTART + DURATION.
          $property = array(
            'value' => iCalUtilityFunctions::_duration2date($dtstart['value'], $duration['value']),
            'params' => $dtstart['params'],
          );
        }
        elseif ($is_all_day) {
          // If this is an all-day event with no end or duration, treat this
          // as a single-day event by emulating DTEND as 1 day after DTSTART.
          $property = $dtstart;
          $property['value']['day'] = $dtstart['value']['day'] + 1;
        }
        else {
          // This event has no end date.
          return NULL;
        }
      }
      elseif ($property_key == 'DTSTART') {
        // DTSTART can only be legally unset in non-VEVENT components.
        if ($vcalendar_component->objName == 'vevent') {
          throw new DateIcalParseException(t('Feed import failed! The VEVENT with UID %uid is invalid: it has no DTSTART.', array('%uid' => $uid)));
        }
        else {
          return NULL;
        }
      }
    }
    
    // When iCalcreator parses a UTC date (one that ends with Z) from an iCal
    // feed, it stores that 'Z' into the $property['value']['tz'] value.
    if (isset($property['value']['tz'])) {
      $property['params']['TZID'] = 'UTC';
    }
    
    if ($is_all_day) {
      if ($property_key == 'DTEND') {
        if ($dtstart === FALSE) {
          // This will almost certainly never happen, but the error message
          // would be incomprehensible without this check.
          throw new DateIcalParseException(t('Feed import failed! The event with UID %uid is invalid: it has a DTEND but no DTSTART!', array('%uid' => $uid)));
        }

        if (module_exists('date_all_day')) {
          // If the Date All Day module is installed, we need to rewind the
          // DTEND by one day, because of the problem with FeedsDateTime
          // mentioned below.
          $prev_day = iCalUtilityFunctions::_duration2date($property['value'], array('day' => -1));
          $property['value'] = $prev_day;
        }
      }

      // FeedsDateTime->setTimezone() ignores timezone changes made to dates
      // with no time element, which means we can't compensate for the Date
      // module's automatic timezone conversion when it writes to the DB. To
      // get around that, we must add 00:00:00 explicitly, even though this
      // causes other problems (see above and below).
      $date_string = sprintf('%d-%d-%d 00:00:00', $property['value']['year'], $property['value']['month'], $property['value']['day']);
      // Use the server's timezone rather than letting it default to UTC.
      // This will help ensure that the date value doesn't get messed up when
      // Date converts its timezone as the value is read from the database.
      // This is *essential* for All Day events, because Date stores them as
      // '2013-10-03 00:00:00' in the database, rather than doing the sensible
      // thing and storing them as '2013-10-03'.
      // NOTE TO MAINTAINERS:
      // This will not work properly if the site is configured to allow users
      // to set their own timezone. Unfortunately, there isn't anything that
      // Date iCal can do about that, as far as I can tell.
      $datetimezone = new DateTimeZone(date_default_timezone_get());
    }
    else {
      // This is a DATE-TIME property.
      $date_string = iCalUtilityFunctions::_format_date_time($property['value']);
      
      // Allow modules to alter the timezone string. This also allows for
      // setting a TZID when one was not originally set for this property.
      $tzid = isset($property['params']['TZID']) ? $property['params']['TZID'] : NULL;
      $context = array(
        'property_key' => $property_key,
        'calendar_component' => $vcalendar_component,
        'calendar' => $this->calendar,
        'feeeds_source' => $this->source,
        'feeds_fetcher_result' => $this->fetcherResult,
      );
      drupal_alter('date_ical_import_timezone', $tzid, $context);
      
      if (isset($tzid)) {
        $datetimezone = $this->_tzid_to_datetimezone($tzid);
      }
      elseif (isset($this->xtimezone)) {
        // No timezone was set on the parsed date property, so if a timezone
        // was detected for the entire iCal feed, use it.
        $datetimezone = $this->xtimezone;
      }
      else {
        $msg = t("No timezone was detected for one or more of the events in this feed, forcing Date iCal to use this server's timezone as a fallback.<br>
            To make timezone-less events use a different timezone, implement hook_date_ical_import_timezone_alter() in a custom module.");
        drupal_set_message($msg, 'status', FALSE);
        $this->source->log('parse', $msg, array(), WATCHDOG_NOTICE);
        $datetimezone = new DateTimeZone(date_default_timezone_get());
      }
    }

    return new FeedsDateTime($date_string, $datetimezone);
  }

  /**
   * Parses multi-value fields, like the CATEGORIES component.
   *
   * @return array
   *   An array of strings contaning the individual values.
   */
  public function parseMultivalueProperty($property_key, $vcalendar_component) {
    // Since we're not telling it to give us the params data, $property will
    // be either FALSE, a string, or an array of strings.
    $property = $vcalendar_component->getProperty($property_key);
    if (empty($property)) {
      // If this multi-value property is being mapped to a Taxonomy field,
      // Feeds will interpret anything besides empty array as an array of
      // empty values (e.g. array('')). This will create a term for that
      // empty value, rather than leaving the field blank.
      return array();
    }
    if (!is_array($property)) {
      $property = array($property);
    }
    return $property;
  }

  /**
   * Format RRULEs, which specify when and how often the event is repeated.
   *
   * @return string
   *   An RRULE string, with EXDATE and RDATE values separated by \n.
   *   This is to make the RRULE compatible with date_repeat_split_rrule().
   */
  public function parseRepeatProperty($property_key, $vcalendar_component) {
    if ($vcalendar_component->getProperty($property_key) === FALSE) {
      return NULL;
    }

    // Due to a few bugs and limitations with Date Repeat, we need to massage
    // the RRULE a bit.
    if (count($vcalendar_component->rrule) > 1) {
      drupal_set_message(t('The event with UID %uid has multiple RRULEs, but the Date Repeat module only supports one. Only the first RRULE in the event will be used.<br>
          If your events need to have a complex repeat pattern, using RDATEs should help.',
        array('%uid' => $vcalendar_component->getProperty('UID'))), 'warning'
      );
      // Date Repeat will get extremely confused if it's sent multiple RRULE
      // values, so we need to manually pare it down to only the first one.
      $vcalendar_component->rrule = array($vcalendar_component->rrule[0]);
    }
    foreach ($vcalendar_component->rrule as &$rrule_data) {
      // RRULEs must have an INTERVAL, or Date Repeat will throw errors.
      if (!isset($rrule_data['value']['INTERVAL'])) {
        $rrule_data['value']['INTERVAL'] = '1';
      }

      if (!isset($rrule_data['value']['COUNT']) && !isset($rrule_data['value']['UNTIL'])) {
        drupal_set_message(t("The event with UID %uid has an indefinitely repeating RRULE, which the Date Repeat module doesn't support.<br>
            As a workaround, Date iCal set the repeat count to @count. This value can be customized in the iCal parser settings.",
          array('%uid' => $vcalendar_component->getProperty('UID'), '@count' => $this->config['indefinite_count'])), 'warning'
        );
        $rrule_data['value']['COUNT'] = $this->config['indefinite_count'];
      }
    }

    $rrule = trim($vcalendar_component->createRrule());
    $rdate = trim($vcalendar_component->createRdate());
    $exrule = trim($vcalendar_component->createExrule());
    $exdate = trim($vcalendar_component->createExdate());
    return "$rrule|$rdate|$exrule|$exdate";
  }

  /**
   * Internal helper function for creating DateTimeZone objects.
   */
  protected function _tzid_to_datetimezone($tzid) {
    try {
      $datetimezone = new DateTimeZone($tzid);
    }
    catch (Exception $e) {
      // In case this is a Windows TZID, read the mapping file to try and
      // convert it to a real TZID.
      $zones = file_get_contents(drupal_get_path('module', 'date_ical') . '/libraries/windowsZones.json');
      $zones_assoc = json_decode($zones, TRUE);
      $windows_to_olson_map = array();
      foreach ($zones_assoc['supplemental']['windowsZones']['mapTimezones'] as $mapTimezone) {
        if ($mapTimezone['mapZone']['_other'] == $tzid) {
          // $mapTimezone['mapZone']['_type'] is space-separated TZIDs.
          $tzids = preg_split('/\s/', $mapTimezone['mapZone']['_type']);
          try {
            // They all have the same UTC offset, so for our purposes we can
            // just take the first one.
            return new DateTimeZone($tzids[0]);
          }
          catch (Exception $e) {
            // If this one also fails, we're out of luck, so just fall through
            // to the regular error report code.
            break;
          }
        }
      }
      
      $tz_wiki = l(t('here'), 'http://en.wikipedia.org/wiki/List_of_tz_database_time_zones#List');
      $help = l(t('README'), 'admin/help/date_ical', array('absolute' => TRUE));
      $msg = t(
          '"@tz" is not a valid timezone (see the TZ column !here), so Date iCal had to fall back to UTC (which is probably wrong!).<br>
          Please read the Date iCal !readme for instructions on how to fix this.',
          array('@tz' => $tzid, '!here' => $tz_wiki, '!readme' => $help)
      );
      $this->source->log('parse', $msg, array(), WATCHDOG_WARNING);
      drupal_set_message($msg, 'warning', FALSE);
      $datetimezone = new DateTimeZone('UTC');
    }
    return $datetimezone;
  }
}