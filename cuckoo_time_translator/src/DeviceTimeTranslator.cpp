#include <boost/bind/bind.hpp>

#include <ros/node_handle.h>
#include <ros/publisher.h>
#include <ros/this_node.h>
#include <dynamic_reconfigure/server.h>

#include <cuckoo_time_translator/OneWayTranslator.h>
#include <cuckoo_time_translator/ConvexHullOwt.h>
#include <cuckoo_time_translator/SwitchingOwt.h>

#include <cuckoo_time_translator/DeviceTimeTranslatorConfig.h>
#include <cuckoo_time_translator/DeviceTimestamp.h>
#include <cuckoo_time_translator/DeviceTimeTranslator.h>
#include <cuckoo_time_translator/KalmanOwt.h>

namespace cuckoo_time_translator {

const std::string DeviceTimeTranslator::kDeviceTimeNamePostfix = "/device_time";

class DeviceTimeTranslator::Impl {
 public:
  Impl(const std::string & nameSpace) : timeTranslator_(NULL), nh_(nameSpace + kDeviceTimeNamePostfix), srv_(nh_)
  {
  }

  ~Impl() {
    delete timeTranslator_;
  }

  OneWayTranslator & getTimeTranslator(){
    updateTranslator();
    return * timeTranslator_;
  }

  FilterAlgorithm getCurrentAlgo() const {
    return currentAlgo_;
  }

  FilterAlgorithm getExpectedAlgo() const {
    return expectedAlgo_;
  }

  void setExpectedAlgo(FilterAlgorithm expectedAlgo) {
    expectedAlgo_ = expectedAlgo;
  }

  ros::Publisher& getDeviceTimePub() {
    return deviceTimePub_;
  }

  dynamic_reconfigure::Server<DeviceTimeTranslatorConfig>& getConfigSrv() {
    return srv_;
  }

  DeviceTimestamp& getMsg() {
    return msg;
  }

  double getExpectedSwitchingTimeSeconds() const {
    return expectedSwitchingTimeSeconds_;
  }

  void setExpectedSwitchingTimeSeconds(double expectedSwitchingTimeSeconds = 0) {
    expectedSwitchingTimeSeconds_ = expectedSwitchingTimeSeconds;
  }

  ros::NodeHandle& getNh() {
    return nh_;
  }

 private:

  template <typename Owt>
  OneWayTranslator* createOwt(){
    if (shouldSwitch(switchingTimeSeconds_)) {
      return new SwitchingOwt(SwitchingOwt::craeteSwitchingOwt<Owt>(switchingTimeSeconds_));
    } else {
      return new Owt;
    }
  }

  void updateTranslator() {
    const FilterAlgorithm expectedAlgo = expectedAlgo_;
    const double expectedSwitchingTimeSeconds = expectedSwitchingTimeSeconds_;
    bool somethingWasUpdated = false;

    if (timeTranslator_ == nullptr || currentAlgo_ != expectedAlgo || (shouldSwitch(expectedSwitchingTimeSeconds) != shouldSwitch(switchingTimeSeconds_))) {
      delete timeTranslator_;
      switchingTimeSeconds_ = expectedSwitchingTimeSeconds;
      somethingWasUpdated = true;

      switch(expectedAlgo.type){
        case FilterAlgorithm::ConvexHull:
          timeTranslator_ = createOwt<ConvexHullOwt>();
          break;
        case FilterAlgorithm::Kalman:
          timeTranslator_ = createOwt<KalmanOwt>();
          break;
        default:
          ROS_ERROR("Unknown device time filter algorithm : %u. Falling back to no filter (NopOwt).", static_cast<unsigned>(expectedAlgo.type));
        case FilterAlgorithm::None:
          timeTranslator_ = new NopOwt();
          break;
      }

      switchingTimeSeconds_ = expectedSwitchingTimeSeconds;
      currentAlgo_ = expectedAlgo;
    }
    else if (expectedSwitchingTimeSeconds != switchingTimeSeconds_) {
      auto switchingOwt = dynamic_cast<SwitchingOwt*>(timeTranslator_);
      if(switchingOwt){
        switchingOwt->setSwitchingTimeSeconds(expectedSwitchingTimeSeconds);
        somethingWasUpdated = true;
      }
      switchingTimeSeconds_ = expectedSwitchingTimeSeconds;
    }

    if(somethingWasUpdated){
      std::stringstream ss;
      timeTranslator_->printNameAndConfig(ss);
      ROS_INFO("Using device time filter : %s.", ss.str().c_str());
    }
  }

  bool shouldSwitch(const double expectedSwitchingTimeSeconds) {
    return expectedSwitchingTimeSeconds > 0;
  }

  OneWayTranslator* timeTranslator_;
  FilterAlgorithm currentAlgo_ = FilterAlgorithm::None, expectedAlgo_ = FilterAlgorithm::None;
  ros::Publisher deviceTimePub_;
  ros::NodeHandle nh_;
  dynamic_reconfigure::Server<DeviceTimeTranslatorConfig> srv_;

  double switchingTimeSeconds_ = 0, expectedSwitchingTimeSeconds_ = 0;
  DeviceTimestamp msg;
};

void DeviceTimeTranslator::configCallback(DeviceTimeTranslatorConfig &config, uint32_t /*level*/)
{
  pImpl_->setExpectedAlgo(FilterAlgorithm::Type(config.filter_algo));
  pImpl_->setExpectedSwitchingTimeSeconds(config.switch_time);
}

DeviceTimeTranslator::DeviceTimeTranslator(const std::string& nameSpace) :
    pImpl_(new Impl(nameSpace))
{
  ROS_INFO("DeviceTimeTranslator is going to publishing device timestamps on %s.", pImpl_->getNh().getNamespace().c_str());
  pImpl_->getDeviceTimePub() = pImpl_->getNh().advertise<DeviceTimestamp>("", 5);
  pImpl_->getConfigSrv().setCallback(boost::bind(&DeviceTimeTranslator::configCallback, this, _1, _2));
}

DeviceTimeTranslator::~DeviceTimeTranslator() {
  delete pImpl_;
}

FilterAlgorithm DeviceTimeTranslator::getCurrentFilterAlgorithm() const {
  return pImpl_->getCurrentAlgo();
}


void DeviceTimeTranslator::setFilterAlgorithm(FilterAlgorithm filterAlgorithm) const {
  pImpl_->setExpectedAlgo(filterAlgorithm);
}

ros::Time DeviceTimeTranslator::update(const TimestampUnwrapper & timestampUnwrapper, const ros::Time & receiveTime, const double offsetSecs) {
  if(!pImpl_) return receiveTime;

  auto & timeTranslator = pImpl_->getTimeTranslator();

  double translatedTime = timeTranslator.updateAndTranslateToLocalTimestamp(RemoteTime(timestampUnwrapper.getTransmitStampSec()), LocalTime(receiveTime.toSec()));

  auto & msg = pImpl_->getMsg();
  if(timestampUnwrapper.hasSeparateTransmitTime()){
    msg.transmit_stamp = timestampUnwrapper.getUnwrappedTransmitStamp().getValue();
    if(timeTranslator.isReady()){
      translatedTime = timeTranslator.translateToLocalTimestamp(RemoteTime(timestampUnwrapper.getEventStampSec()));
    }
  }
  if (timeTranslator.isReady()){
    msg.header.stamp.fromSec(translatedTime);
  } else {
    msg.header.stamp = receiveTime;
  }

  msg.header.stamp += ros::Duration(offsetSecs);

  if(pImpl_->getDeviceTimePub().getNumSubscribers()){
    msg.event_stamp = timestampUnwrapper.getUnwrappedEventStamp().getValue();
    msg.receive_time = receiveTime;
    msg.offset_secs = offsetSecs;
    msg.filter_algorithm = uint8_t(pImpl_->getCurrentAlgo().type);
    pImpl_->getDeviceTimePub().publish(msg);
  }
  ROS_DEBUG("Device time %llu + receive time %10.6f sec mapped to %10.6f sec (receive - translated = %.3f ms).", static_cast<long long unsigned>(timestampUnwrapper.getUnwrappedEventStamp().getValue()), receiveTime.toSec(), translatedTime, (receiveTime.toSec() - translatedTime) * 1000);
  return msg.header.stamp;
}

ros::Time DeviceTimeTranslator::translate(const TimestampUnwrapper & timestampUnwrapper, UnwrappedStamp unwrappedEventStamp) const {
  return ros::Time(pImpl_->getTimeTranslator().translateToLocalTimestamp(RemoteTime(timestampUnwrapper.stampToSec(unwrappedEventStamp))));
}

template <typename Unwrapper>
DeviceTimeUnwrapperAndTranslator<Unwrapper>::DeviceTimeUnwrapperAndTranslator(const UnwrapperClockParameters & clockParameters, const std::string & nameSpace) :
    timestampUnwrapper(clockParameters),
    translator(nameSpace)
{
}

template <typename Unwrapper>
ros::Time DeviceTimeUnwrapperAndTranslator<Unwrapper>::update(Timestamp eventStamp, const ros::Time & receiveTime, double offset) {
  timestampUnwrapper.updateWithNewEventStamp(eventStamp);
  return translator.update(timestampUnwrapper, receiveTime, offset);
}

template <typename Unwrapper>
ros::Time DeviceTimeUnwrapperAndTranslatorWithTransmitTime<Unwrapper>::update(Timestamp eventStamp, Timestamp transmitStamp, const ros::Time & receiveTime, double offset) {
  this->timestampUnwrapper.updateWithNewEventStamp(eventStamp);
  this->timestampUnwrapper.updateWithNewTransmitStamp(transmitStamp);
  return this->translator.update(this->timestampUnwrapper, receiveTime, offset);
}

template <typename Unwrapper>
ros::Time DeviceTimeUnwrapperAndTranslator<Unwrapper>::translate(UnwrappedStamp unwrappedStamp) const {
  return translator.translate(timestampUnwrapper, unwrappedStamp);
}

template <typename Unwrapper>
UnwrappedStamp DeviceTimeUnwrapperAndTranslator<Unwrapper>::unwrapEventStamp(typename Unwrapper::Timestamp eventStamp) {
  timestampUnwrapper.updateWithNewEventStamp(eventStamp);
  return timestampUnwrapper.getUnwrappedEventStamp();
}

template<typename Unwrapper_>
DeviceTimeUnwrapperAndTranslatorWithTransmitTime<Unwrapper_>::DeviceTimeUnwrapperAndTranslatorWithTransmitTime(const UnwrapperClockParameters& clockParameters, const std::string& nameSpace) :
  DeviceTimeUnwrapperAndTranslator<Unwrapper_>(clockParameters, nameSpace)
{
}

template <typename Unwrapper>
UnwrappedStamp DeviceTimeUnwrapperAndTranslatorWithTransmitTime<Unwrapper>::unwrapTransmitStamp(Timestamp eventStamp) {
  this->timestampUnwrapper.updateWithNewTransmitStamp(eventStamp);
  return this->timestampUnwrapper.getUnwrappedTransmitStamp();
}

template class DeviceTimeUnwrapperAndTranslator<TimestampUnwrapperEventOnly>;
template class DeviceTimeUnwrapperAndTranslator<TimestampPassThrough>;
template class DeviceTimeUnwrapperAndTranslatorWithTransmitTime<TimestampUnwrapperEventAndTransmit>;

}